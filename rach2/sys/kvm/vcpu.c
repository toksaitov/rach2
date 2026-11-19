/*
 * vcpu.c: a VM's vCPUs: the vCPU descriptor, its run pages mapped into the
 * caller, KVM_RUN's loop, and the ioctls of the vCPU's state. Each ioctl
 * holds the vCPU's mutex; KVM_RUN runs without the big lock and the others
 * under it, as kvm.c's VM ioctls do.
 */

#include "kvm.h"

#include "../arch/arch.h"
#include "../fs/fd.h"
#include "../fs/file.h"
#include "../lib/kerrno.h"
#include "../lib/kirq.h"
#include "../lib/kstring.h"
#include "../mm/kheap.h"
#include "../mm/paging.h"
#include "../mm/usercopy.h"
#include "../mm/vm.h"
#include "../proc/process.h"
#include "../proc/sched.h"
#include "../proc/smp.h"

/* The XSAVE image's header, after the 512 bytes of the legacy region. */
#define XSAVE_HDR       512u
#define XFEATURE_X87    0x1ull
#define XFEATURE_SSE    0x2ull
#define XFEATURE_YMM    0x4ull
#define XFEATURE_AVX512 0xE0ull

/* The MXCSR bits a guest may set: Linux takes the processor's
 * MXCSR_MASK, 0xFFFF on every processor with DAZ. */
#define MXCSR_MASK 0xFFFFu

#define CR0_PE 0x00000001ull
#define CR0_ET 0x00000010ull
#define CR0_NW 0x20000000ull
#define CR0_CD 0x40000000ull
#define CR0_PG 0x80000000ull

#define CR4_PAE      (1ull << 5)
#define CR4_MCE      (1ull << 6)
#define CR4_UMIP     (1ull << 11)
#define CR4_LA57     (1ull << 12)
#define CR4_VMXE     (1ull << 13)
#define CR4_SMXE     (1ull << 14)
#define CR4_FSGSBASE (1ull << 16)
#define CR4_PCIDE    (1ull << 17)
#define CR4_OSXSAVE  (1ull << 18)
#define CR4_SMEP     (1ull << 20)
#define CR4_SMAP     (1ull << 21)
#define CR4_PKE      (1ull << 22)
/* The bits below UMIP that every processor KVM runs on has: VME to
 * OSXMMEXCPT. */
#define CR4_BASE     0x7FFull

#define EFER_LME 0x100ull
#define EFER_LMA 0x400ull

#define APIC_BASE_DEFAULT 0xFEE00000ull
#define APIC_BASE_BSP     0x100ull
#define APIC_BASE_X2APIC  0x400ull
#define APIC_BASE_ENABLE  0x800ull

#define MCI_STATUS_VAL  (1ull << 63)
#define MCI_STATUS_OVER (1ull << 62)
#define MCI_STATUS_UC   (1ull << 61)
#define MCG_STATUS_MCIP (1ull << 2)

#define DB_VECTOR  1
#define NMI_VECTOR 2
#define BP_VECTOR  3
#define MC_VECTOR  18

/* DR6's bits that read as 1 and DR7's bit 10, at reset. */
#define DR6_RESET 0xFFFF0FF0ull
#define DR7_RESET 0x400ull

/* The bits of a signal mask, signo - 1. */
#define SIGKILL_BIT (1ull << 8)
#define SIGSTOP_BIT (1ull << 18)

/* The highest MP state Linux knows, KVM_MP_STATE_SUSPENDED. */
#define KVM_MP_STATE_MAX 10u

/** Take `v` for one ioctl, waiting while another holds it, as Linux's
 * kvm_vcpu_ioctl takes vcpu->mutex (virt/kvm/kvm_main.c:4489): 0 once held,
 * or -EINTR with `v` not taken when SIGKILL comes for the waiter. No other
 * signal ends the wait. The holder may be a thread of another process that
 * shares the VM's memory and descriptors (CLONE_VM | CLONE_FILES without
 * CLONE_THREAD), which the waiter's SIGKILL does not reach. */
static int vcpu_lock(struct kvm_vcpu *v)
{
    return mutex_lock_killable(&v->mutex);
}

/** Let go of `v`. */
static void vcpu_unlock(struct kvm_vcpu *v)
{
    mutex_unlock(&v->mutex);
}

/** Write back the parts `parts` of `v`'s state from its backend. */
static void vcpu_save(struct kvm_vcpu *v, uint32_t parts)
{
    if (kvm_backend && kvm_backend->save && v->has_run) {
        kvm_backend->save(v, parts);
    }
}

void kvm_vcpu_fpu_sync(struct kvm_vcpu *v)
{
    vcpu_save(v, KVM_PART_FPU);
}

/** Give the host back what the backend left the guest's, with `fpu` the
 * floating-point registers too: the backend's `put`, with interrupts
 * off. */
static void vcpu_put(struct kvm_vcpu *v, int fpu)
{
    uint32_t flags;

    if (kvm_backend && kvm_backend->put) {
        flags = kirq_save();
        kvm_backend->put(v, fpu);
        kirq_restore(flags);
    }
}

/** A segment of the reset state. */
static void reset_seg(struct kvm_segment *s, uint16_t sel, uint64_t base, uint8_t type)
{
    k_memset(s, 0, sizeof(*s));
    s->selector = sel;
    s->base = base;
    s->limit = 0xFFFF;
    s->type = type;
    s->present = 1;
    s->s = 1;
}

/**
 * The architectural state a reset and an INIT both give, as Linux's
 * kvm_vcpu_reset: real mode at 0xFFFF0 below the reset vector's segment,
 * EDX the family, model and stepping of the guest's CPUID, or 0x600 before
 * KVM_SET_CPUID2, CR0 with ET, no event waiting, and the debug registers at
 * reset; the APIC base as it was. CR0's CD and NW are set at a power-on
 * reset and kept as they were at an INIT, `init` nonzero, as Linux's
 * (arch/x86/kvm/x86.c:11752-11761); the record's CD and NW are the guest's
 * at an exit, since VMX keeps them the host's and SVM reads CR0 back at
 * each exit it does not intercept.
 *
 * The segments are built aside and copied in whole, so that the APIC base
 * never reads other than it was: another processor's delivery reads it
 * without a lock (vlapic.c's `hw_enabled` and `x2apic`), and a SIPI sent
 * just after the INIT that this reset takes would otherwise find the APIC
 * off and be dropped, leaving the vCPU waiting for it for ever. Linux's
 * INIT leaves the base alone (arch/x86/kvm/lapic.c:2814-2826) and its
 * delivery reads the APIC map, rebuilt under `apic_map_lock`
 * (lapic.c:385-495, 1234-1269).
 */
static void reset_arch(struct kvm_vcpu *v, int init)
{
    const struct kvm_cpuid_entry2 *l1 = kvm_cpuid_find(v, 1, 0);
    struct kvm_sregs s;

    k_memset(&v->regs, 0, sizeof(v->regs));
    k_memset(&s, 0, sizeof(s));
    v->regs.rdx = l1 ? l1->eax : 0x600;
    v->regs.rip = 0xFFF0;
    v->regs.rflags = 0x2;
    reset_seg(&s.cs, 0xF000, 0xFFFF0000ull, 0xB);
    reset_seg(&s.ds, 0, 0, 0x3);
    reset_seg(&s.es, 0, 0, 0x3);
    reset_seg(&s.fs, 0, 0, 0x3);
    reset_seg(&s.gs, 0, 0, 0x3);
    reset_seg(&s.ss, 0, 0, 0x3);
    reset_seg(&s.tr, 0, 0, 0xB);
    s.tr.s = 0;
    reset_seg(&s.ldt, 0, 0, 0x2);
    s.ldt.s = 0;
    s.gdt.limit = 0xFFFF;
    s.idt.limit = 0xFFFF;
    s.cr0 = CR0_ET | (init ? v->sregs.cr0 & (CR0_CD | CR0_NW) : CR0_CD | CR0_NW);
    s.apic_base = v->sregs.apic_base;
    v->sregs = s;
    k_memset(&v->events, 0, sizeof(v->events));
    v->nmi_queued = 0;
    k_memset(&v->debugregs, 0, sizeof(v->debugregs));
    v->debugregs.dr6 = DR6_RESET;
    v->debugregs.dr7 = DR7_RESET;
}

/**
 * Give `v` the state of a processor at power-on, as Linux's
 * kvm_vcpu_reset gives it: the state an INIT gives, the local APIC at its
 * default address, the boot vCPU's marked, the x87 and SSE units at their
 * initial state with XCR0 naming x87 alone, the MSRs at reset, and the TSC
 * at 0.
 */
static void vcpu_reset(struct kvm_vcpu *v)
{
    uint64_t bv = XFEATURE_X87 | XFEATURE_SSE;

    reset_arch(v, 0);
    v->sregs.apic_base = APIC_BASE_DEFAULT | APIC_BASE_ENABLE;
    if (v->id == v->vm->boot_cpu_id) {
        v->sregs.apic_base |= APIC_BASE_BSP;
    }
    k_memset(v->xsave, 0, KVM_XSAVE_SIZE);
    v->xsave[0] = 0x7F; /* FCW 0x37F */
    v->xsave[1] = 0x03;
    v->xsave[24] = 0x80; /* MXCSR 0x1F80 */
    v->xsave[25] = 0x1F;
    k_memcpy(v->xsave + XSAVE_HDR, &bv, sizeof(bv));
    v->xcr0 = XFEATURE_X87;
    kvm_cpuid_xsave_sizes(v, v->cpuid, v->cpuid_nent);
    k_memset(&v->guest_debug, 0, sizeof(v->guest_debug));
    v->mp_state = KVM_MP_STATE_RUNNABLE;
    v->tsc_offset = -(int64_t)kvm_tsc_scale(v, arch_cycles());
    kvm_msr_reset(v);
    kvm_pmu_init(v);
    v->dirty = KVM_PART_ALL;
}

void kvm_vcpu_init_reset(struct kvm_vcpu *v)
{
    reset_arch(v, 1);
    kvm_pmu_reset(v);
    /* As Linux's kvm_vcpu_reset at INIT: the steal time and the kvmclock
     * are off. */
    v->st_msr = 0;
    v->pv_time = 0;
    /* SPEC_CTRL and VIRT_SPEC_CTRL are cleared, as Linux's vmx_vcpu_reset
     * and svm_vcpu_reset do at INIT too (vmx/vmx.c:4909, svm/svm.c:1402-1403);
     * each entry loads them from here. */
    v->spec_ctrl = 0;
    v->virt_spec_ctrl = 0;
    v->dirty |= KVM_PART_REGS | KVM_PART_SREGS | KVM_PART_EVENTS | KVM_PART_DEBUG | KVM_PART_CR8;
}

uint8_t *kvm_map_pages(uint32_t count, uintptr_t *pa)
{
    uint8_t *p = kalloc_contig(count, pa);

    if (p) {
        k_memset(p, 0xFF, (size_t)count * 4096u);
    }
    return p;
}

/** The page `pgoff` of `v`'s mapping in the kernel: its run page, its page
 * of port I/O data, or the VM's ring of coalesced writes at
 * KVM_COALESCED_MMIO_PAGE_OFFSET, and its dirty ring at
 * KVM_DIRTY_LOG_PAGE_OFFSET; NULL outside them. */
static uint8_t *mapped_page(struct kvm_vcpu *v, uint32_t pgoff)
{
    if (pgoff < KVM_RUN_PAGES) {
        return (uint8_t *)v->run + (uintptr_t)pgoff * 4096u;
    }
    if (pgoff == KVM_COALESCED_MMIO_PAGE_OFFSET) {
        return (uint8_t *)v->vm->ring;
    }
    if (pgoff >= KVM_DIRTY_LOG_PAGE_OFFSET &&
        pgoff - KVM_DIRTY_LOG_PAGE_OFFSET < v->vm->dirty_ring_size / 4096u) {
        return (uint8_t *)v->dirty_ring + (uintptr_t)(pgoff - KVM_DIRTY_LOG_PAGE_OFFSET) * 4096u;
    }
    return NULL;
}

/** The page `pgoff` of the mapping of the vCPU `obj`, for a fault of a
 * mapping of it: 0, or -1 past its pages, which raises SIGBUS. Each page's
 * frame is read from the kernel's tables, since the heap's pages are
 * contiguous only in virtual memory. */
static int run_page(void *obj, uint32_t pgoff, uintptr_t *phys)
{
    uint8_t *page = mapped_page((struct kvm_vcpu *)obj, pgoff);

    if (!page) {
        return -1;
    }
    *phys = paging_virt_to_phys((uintptr_t)page);
    return *phys ? 0 : -1;
}

/** The last mapping of the vCPU `obj`'s run pages is gone: the reference
 * it held to the VM goes. */
static void run_put(void *obj)
{
    kvm_count(KVM_COUNT_RUN_MAPPING, -1);
    kvm_vm_put(((struct kvm_vcpu *)obj)->vm);
}

static const struct vm_device_ops run_vm_ops = {run_page, run_put, NULL, 0};

/* A private mapping of a vCPU's run pages: the record its region names,
 * which holds a reference to the VM. */
struct run_copy {
    struct vm_file vf;
    struct kvm_vcpu *vcpu;
};

/** Fill the frame `phys` of a private mapping `f` with a copy of the run
 * page `pgoff` as it is now: 0, or -1 past the pages, which raises SIGBUS
 * as on Linux. */
static int run_copy_fill(struct vm_file *f, uint32_t pgoff, uintptr_t phys)
{
    const uint8_t *from = mapped_page(((struct run_copy *)f)->vcpu, pgoff);
    uint8_t *page;

    if (!from) {
        return -1;
    }
    page = kmap_temp(phys);
    if (!page) {
        return -1;
    }
    k_memcpy(page, from, 4096u);
    kunmap_temp(page);
    return 0;
}

/** The last region of a private mapping `f` is gone: its reference to the
 * VM goes with the record. */
static void run_copy_release(struct vm_file *f)
{
    struct run_copy *c = (struct run_copy *)f;

    kvm_count(KVM_COUNT_RUN_MAPPING, -1);
    kvm_vm_put(c->vcpu->vm);
    kfree(c->vf.path);
    kfree(c);
}

/**
 * mmap of a vCPU: its run pages. A shared mapping maps them cached, and a
 * fork's child shares it, as Linux's does. A private one, which Linux also
 * allows, gives each page as it was at the first touch, as a private
 * mapping of a file does here (mm/vm.h). Either holds a reference to the
 * VM, so the pages outlive the descriptor. A mapping over the dirty ring
 * must be shared and non-executable, as Linux's kvm_vcpu_mmap checks.
 * 0, -EINVAL for such a forbidden mapping, -ENOMEM, or vm_device_map's error.
 */
static int32_t vcpu_mmap(fd_entry_t *e, struct vma *vma)
{
    struct kvm_vcpu *v = (struct kvm_vcpu *)e->priv;
    struct run_copy *c;
    int32_t r;
    uint64_t end = (uint64_t)vma->pgoff + (vma->end - vma->start) / 4096u;

    if (v->vm->dirty_ring_size &&
        vma->pgoff < KVM_DIRTY_LOG_PAGE_OFFSET + v->vm->dirty_ring_size / 4096u &&
        end > KVM_DIRTY_LOG_PAGE_OFFSET && (vma->type != VM_SHARED || (vma->prot & VM_PROT_EXEC))) {
        return -EINVAL;
    }

    if (vma->type != VM_SHARED) {
        c = kzalloc(sizeof(*c));
        if (!c) {
            return -ENOMEM;
        }
        c->vf.refs = 1;
        c->vf.fill = run_copy_fill;
        c->vf.release = run_copy_release;
        c->vcpu = v;
        kvm_vm_get(v->vm);
        kvm_count(KVM_COUNT_RUN_MAPPING, 1);
        vma->file = &c->vf;
        return 0;
    }
    kvm_vm_get(v->vm);
    r = vm_device_map(vma, &run_vm_ops, v, 0);
    if (r != 0) {
        kvm_vm_put(v->vm);
    } else {
        kvm_count(KVM_COUNT_RUN_MAPPING, 1);
    }
    return r;
}

/** The last close of a vCPU's descriptor: its reference to the VM goes,
 * and the vCPU lives on with the VM, as on Linux. Called without the big
 * lock, which the VM's release takes. */
static void vcpu_close(fd_entry_t *e)
{
    bkl_enter();
    kvm_vm_put(((struct kvm_vcpu *)e->priv)->vm);
    e->priv = NULL;
    bkl_exit();
}

/* The descriptor's calls run without the big lock (FILE_OPS_UNLOCKED):
 * kvm_vcpu_ioctl takes it for all but KVM_RUN, and mmap runs under it. */
static const struct file_ops vcpu_ops = {NULL,       NULL,      NULL, kvm_vcpu_ioctl,
                                         vcpu_close, vcpu_mmap, NULL, FILE_OPS_UNLOCKED};

void kvm_vcpu_free(struct kvm_vcpu *v)
{
    if (!v) {
        return;
    }
    if (kvm_backend && kvm_backend->vcpu_free) {
        kvm_backend->vcpu_free(v);
    }
    kvm_lapic_free(v);
    kvm_hv_vcpu_free(v);
    kvm_xen_vcpu_free(v);
    if (v->run) {
        kfree_pages(v->run, KVM_RUN_PAGES);
    }
    if (v->dirty_ring) {
        kfree_pages(v->dirty_ring, v->vm->dirty_ring_size / 4096u);
    }
    kfree(v->xsave_mem);
    kfree(v->cpuid);
    kvm_count(KVM_COUNT_VCPU, -1);
    kfree(v);
}

static uint64_t cr4_reserved_by(const struct kvm_vcpu *v);

int32_t kvm_vcpu_create(struct kvm_vm *vm, uint32_t id)
{
    struct kvm_vcpu *v;
    fd_entry_t *e;
    uint32_t i;
    int32_t r;
    int fd;

    if (id >= KVM_MAX_VCPU_IDS || vm->nr_vcpus >= KVM_MAX_VCPUS) {
        return -EINVAL;
    }
    /* The first vCPU settles the bound of the ids, as Linux's
     * kvm_arch_vcpu_precreate. */
    if (!vm->max_vcpu_ids) {
        vm->max_vcpu_ids = KVM_MAX_VCPU_IDS;
    }
    if (id >= vm->max_vcpu_ids) {
        return -EINVAL;
    }
    for (i = 0; i < vm->nr_vcpus; i++) {
        if (vm->vcpus[i]->id == id) {
            return -EEXIST;
        }
    }
    v = kzalloc(sizeof(*v));
    if (!v) {
        return -ENOMEM;
    }
    kvm_count(KVM_COUNT_VCPU, 1);
    v->vm = vm;
    v->id = id;
    v->index = vm->nr_vcpus;
    v->cpu = -1;
    v->clock_cpu = -1;
    v->xs_row = -1;
    v->poll_control = 1; /* polling allowed, as Linux's kvm_arch_vcpu_postcreate sets it */
    v->hlt_in_guest = (vm->disable_exits & KVM_X86_DISABLE_EXITS_HLT) != 0;
    v->cr4_reserved = cr4_reserved_by(NULL);
    k_memcpy(v->name, "kvm-vcpu:", 9);
    kvm_decimal(v->name + 9, id);
    kvm_stats_id(v->stats_id, v);
    v->anon.name = v->name;
    mutex_init(&v->mutex, "kvm_vcpu");
    r = -ENOMEM;
    v->run = kalloc_pages(KVM_RUN_PAGES, NULL);
    v->xsave_mem = kmalloc(KVM_XSAVE_SIZE + 64u);
    if (!v->run || !v->xsave_mem || kvm_xen_vcpu_init(v) != 0) {
        goto fail;
    }
    k_memset(v->run, 0, KVM_RUN_PAGES * 4096u);
    if (vm->dirty_ring_size) {
        v->dirty_ring = kalloc_pages(vm->dirty_ring_size / 4096u, NULL);
        if (!v->dirty_ring) {
            goto fail;
        }
        k_memset(v->dirty_ring, 0, vm->dirty_ring_size);
    }
    v->pio = (uint8_t *)v->run + KVM_PIO_PAGE_OFFSET * 4096u;
    v->xsave = (uint8_t *)(((uintptr_t)v->xsave_mem + 63u) & ~(uintptr_t)63u);
    (void)kvm_tsc_set_khz(v, vm->default_tsc_khz);
    vcpu_reset(v);
    if (vm->irqchip != KVM_IRQCHIP_NONE) {
        r = kvm_lapic_create(v);
        if (r != 0) {
            goto fail;
        }
    }
    if (kvm_backend->vcpu_init) {
        r = kvm_backend->vcpu_init(v);
        if (r != 0) {
            goto fail;
        }
    }
    fd = fd_alloc(current_proc->fds);
    if (fd < 0) {
        r = fd;
        goto fail;
    }
    /* The run loops of the other vCPUs read the array without the big
     * lock: the entry is whole before the count takes it in. */
    vm->vcpus[vm->nr_vcpus] = v;
    k_barrier();
    vm->nr_vcpus++;
    kvm_vm_get(vm);
    kvm_tsc_sync(v, NULL);
    e = fd_slot(current_proc->fds, fd);
    e->type = FD_TYPE_ANON;
    e->ops = &vcpu_ops;
    e->priv = v;
    e->flags = FD_FLAG_READ | FD_FLAG_WRITE | FD_FLAG_CLOEXEC;
    fd_install(current_proc->fds, fd);
    return fd;
fail:
    kvm_vcpu_free(v);
    return r;
}

/** OR `bits` into `v`'s requests atomically, since another processor's
 * kick may set one at the same time. */
static void req_set(struct kvm_vcpu *v, uint32_t bits)
{
    uint32_t old;

    do {
        old = v->requests;
    } while (arch_atomic_cmpxchg((volatile unsigned int *)&v->requests, old, old | bits) != old);
}

int kvm_vcpu_test_clear(struct kvm_vcpu *v, uint32_t request)
{
    uint32_t old;

    do {
        old = v->requests;
        if (!(old & request)) {
            return 0;
        }
    } while (arch_atomic_cmpxchg((volatile unsigned int *)&v->requests, old, old & ~request) !=
             old);
    return 1;
}

void kvm_vcpu_kick(struct kvm_vcpu *v, uint32_t request)
{
    req_set(v, request);
    if (v->mode == KVM_MODE_IN_GUEST && v->cpu >= 0 && (unsigned int)v->cpu != arch_cpu_id()) {
        smp_kick(v->cpu);
    }
}

void kvm_timer_arm(struct kvm_vcpu *v, uint64_t ns)
{
    if (ns && (!v->timer_ns || ns < v->timer_ns)) {
        v->timer_ns = ns;
    }
}

/** The earlier of two deadlines, 0 standing for none. */
static uint64_t earlier(uint64_t a, uint64_t b)
{
    return !a || (b && b < a) ? b : a;
}

void kvm_vcpu_timers(struct kvm_vcpu *v)
{
    uint64_t next = 0, clock, xen;

    if (v->timer_ns && kvm_host_ns() >= v->timer_ns) {
        req_set(v, KVM_REQ_TIMER);
    }
    if (!kvm_vcpu_test_clear(v, KVM_REQ_TIMER)) {
        return;
    }
    xen = kvm_xen_timers(v);
    if (v->vm->irqchip != KVM_IRQCHIP_NONE) {
        kvm_lapic_timer_check(v);
        kvm_pit_check(v->vm);
        kvm_hv_stimers(v);
        next = kvm_lapic_next_ns(v);
    }
    clock = kvm_clock_next(v);
    if (clock && kvm_host_ns() >= clock) {
        /* The write moves the resync on (`kvm_timer_arm`). */
        req_set(v, KVM_REQ_CLOCK_UPDATE);
        clock = 0;
    }
    v->timer_ns = earlier(earlier(next, clock), xen);
}

/** Queue the exception `nr` for `v`, as Linux replaces a benign exception
 * waiting with a new one, which the instruction raises again when run. */
static void queue_exception(struct kvm_vcpu *v, uint8_t nr)
{
    v->events.exception.injected = 1;
    v->events.exception.pending = 0;
    v->events.exception.nr = nr;
    v->events.exception.has_error_code = 0;
    v->events.exception.error_code = 0;
    v->dirty |= KVM_PART_EVENTS;
    req_set(v, KVM_REQ_EVENT);
}

/** Fill the run page's fields every return of KVM_RUN gives, as Linux's
 * post_kvm_run_save. */
static void run_post(struct kvm_vcpu *v)
{
    struct kvm_run *run = v->run;

    run->if_flag = (uint8_t)(v->if_flag != 0);
    run->apic_base = v->sregs.apic_base;
    if (v->vm->irqchip != KVM_IRQCHIP_NONE) {
        run->cr8 = kvm_lapic_cr8(v);
        run->ready_for_interrupt_injection = (uint8_t)kvm_lapic_ready(v);
    } else {
        run->cr8 = v->sregs.cr8;
        run->ready_for_interrupt_injection =
            (uint8_t)(v->irq_allowed && !v->events.interrupt.injected);
    }
    run->flags = 0;
}

/** Give the backend the caller's answer to the read of the last exit, the
 * IN's bytes from the port I/O page or the MMIO read's from the run page,
 * or the news that the caller has written the last exit's OUT. */
static void run_complete(struct kvm_vcpu *v)
{
    struct kvm_exit *ex = &v->exit;
    uint32_t n;

    if (ex->kind == KVM_X_IO && ex->io.direction == KVM_EXIT_IO_IN) {
        n = ex->io.count * ex->io.size;
        k_memcpy(ex->data, v->pio, n < sizeof(ex->data) ? n : sizeof(ex->data));
    } else if (ex->kind == KVM_X_MMIO) {
        k_memcpy(ex->mmio.data, v->run->u.mmio.data, sizeof(ex->mmio.data));
    }
    v->pending = 0;
    if (kvm_backend->complete) {
        kvm_backend->complete(v);
    }
}

/** Give the caller the RDMSR or WRMSR of the KVM_X_MSR exit in `v->exit`,
 * which `kvm_msr_complete` finishes with its answer. */
static void run_msr(struct kvm_vcpu *v)
{
    struct kvm_run *run = v->run;

    run->exit_reason = v->exit.msr.write ? KVM_EXIT_X86_WRMSR : KVM_EXIT_X86_RDMSR;
    k_memset(&run->u.msr, 0, sizeof(run->u.msr));
    run->u.msr.reason = v->exit.msr.reason;
    run->u.msr.index = v->exit.msr.index;
    run->u.msr.data = v->exit.msr.data;
    v->msr_pending = 1;
}

/** Give the caller the port I/O of the KVM_X_IO exit in `v->exit`, unless
 * a device in the kernel takes it: 1 with the run page and the data page
 * filled, or 0 when the kernel took it. */
static int32_t run_io(struct kvm_vcpu *v)
{
    struct kvm_run *run = v->run;
    struct kvm_exit *ex = &v->exit;
    uint32_t n;
    int out = ex->io.direction == KVM_EXIT_IO_OUT;

    v->stat[KVM_VCPU_STAT_IO_EXITS]++;
    if (kvm_irq_io(v) || (out && kvm_iobus_write(v))) {
        /* A device in the kernel took the OUT: it is done. */
        if (out && kvm_backend->complete) {
            kvm_backend->complete(v);
        }
        return 0;
    }
    run->exit_reason = KVM_EXIT_IO;
    run->u.io = ex->io;
    run->u.io.data_offset = KVM_PIO_PAGE_OFFSET * 4096u;
    n = ex->io.count * ex->io.size;
    if (n > 4096u) {
        n = 4096u;
    }
    if (out) {
        k_memcpy(v->pio, ex->data, n);
    }
    v->pending = 1;
    return 1;
}

/**
 * Give the caller the exit in `v->exit`: 1 when it goes to the caller,
 * with the run page filled, 0 when the kernel took it and the guest enters
 * again, or -errno.
 */
static int32_t run_exit(struct kvm_vcpu *v)
{
    struct kvm_run *run = v->run;
    struct kvm_exit *ex = &v->exit;
    int r;

    switch (ex->kind) {
    case KVM_X_AGAIN:
        return 0;
    case KVM_X_IO:
        return run_io(v);
    case KVM_X_MMIO:
        if (kvm_lapic_mmio(v) || kvm_irq_io(v) || (ex->mmio.is_write && kvm_iobus_write(v))) {
            return 0;
        }
        v->stat[KVM_VCPU_STAT_MMIO_EXITS]++;
        run->exit_reason = KVM_EXIT_MMIO;
        run->u.mmio = ex->mmio;
        v->pending = !ex->mmio.is_write;
        return 1;
    case KVM_X_HLT:
        v->stat[KVM_VCPU_STAT_HALT_EXITS]++;
        r = kvm_lapic_halt(v);
        if (r < 0) {
            run->exit_reason = KVM_EXIT_INTR;
            return r;
        }
        if (r == 0) {
            return 0;
        }
        run->exit_reason = KVM_EXIT_HLT;
        return 1;
    case KVM_X_SHUTDOWN:
        run->exit_reason = KVM_EXIT_SHUTDOWN;
        return 1;
    case KVM_X_IRQ_WINDOW:
        v->stat[KVM_VCPU_STAT_IRQ_WINDOW_EXITS]++;
        if (v->vm->irqchip != KVM_IRQCHIP_NONE) {
            /* The window the APIC's interrupt waited for; the loop gives
             * it to the caller too when the caller asked. */
            req_set(v, KVM_REQ_EVENT);
            return 0;
        }
        run->exit_reason = KVM_EXIT_IRQ_WINDOW_OPEN;
        return 1;
    case KVM_X_INTERNAL:
        run->exit_reason = KVM_EXIT_INTERNAL_ERROR;
        run->u.internal = ex->internal;
        if (run->u.internal.ndata > 16u) {
            run->u.internal.ndata = 16u;
        }
        return 1;
    case KVM_X_FAIL_ENTRY:
        run->exit_reason = KVM_EXIT_FAIL_ENTRY;
        run->u.fail_entry.hardware_entry_failure_reason = ex->hw_reason;
        run->u.fail_entry.cpu = (uint32_t)v->cpu;
        return 1;
    case KVM_X_DEBUG:
        run->exit_reason = KVM_EXIT_DEBUG;
        run->u.debug = ex->debug;
        return 1;
    case KVM_X_SYSTEM_EVENT:
        run->exit_reason = KVM_EXIT_SYSTEM_EVENT;
        run->u.system_event = ex->system_event;
        return 1;
    case KVM_X_HYPERV:
        run->exit_reason = KVM_EXIT_HYPERV;
        run->u.hyperv = ex->hyperv;
        return 1;
    case KVM_X_XEN:
        run->exit_reason = KVM_EXIT_XEN;
        run->u.xen = ex->xen;
        return 1;
    case KVM_X_MSR:
        run_msr(v);
        return 1;
    default:
        break;
    }
    run->exit_reason = KVM_EXIT_INTERNAL_ERROR;
    k_memset(&run->u.internal, 0, sizeof(run->u.internal));
    run->u.internal.suberror = KVM_INTERNAL_ERROR_UNEXPECTED_EXIT_REASON;
    run->u.internal.ndata = 1;
    run->u.internal.data[0] = (uint64_t)ex->kind;
    return 1;
}

/** Whether `v` must not enter before the run loop has done its work: a
 * request the loop has not taken (KVM_REQ_NOENTER), or a stop or start of
 * the host's clock since the VM's master clock was decided
 * (`kvm_clock_host_moved`), which Linux posts to every vCPU as
 * KVM_REQ_MASTERCLOCK_UPDATE (arch/x86/kvm/x86.c:9166-9169) and which this
 * test turns into that request, since the host's clock reaches no VM. With
 * the vCPU IN_GUEST: at the entry after a barrier, in the fast path after
 * an exit, which the stop's interrupt forces (`vclock_kick`). */
static int entry_waits(struct kvm_vcpu *v)
{
    if (kvm_clock_host_moved(v->vm)) {
        req_set(v, KVM_REQ_MASTERCLOCK_UPDATE);
    }
    return (v->requests & KVM_REQ_NOENTER) != 0;
}

/** Whether the run loop has work before `v`'s next entry that the fast
 * path cannot do, as Linux's kvm_vcpu_exit_request (arch/x86/kvm/x86.c:
 * 1715-1720) ends its fast path for a request, a signal or a switch the
 * scheduler asked for (x86.c:10586): a request the loop answers or a
 * change of the host's clock (`entry_waits`), the flags of a signal and of
 * a switch, which `run_signal` and `sched_take_preempt` read, and the
 * caller's wait for an interrupt window, which the loop reports after each
 * exit it handles; a PMI the last run left is a request (KVM_REQ_PMI). With
 * interrupts off, the vCPU IN_GUEST. */
static int fast_ends(struct kvm_vcpu *v)
{
    return entry_waits(v) || *(volatile uint32_t *)&current_proc->sig_woken ||
           current_proc->need_resched ||
           (v->vm->irqchip == KVM_IRQCHIP_SPLIT && v->run->request_interrupt_window);
}

/**
 * Enter the guest, with interrupts off, as the backend's `enter` needs, until
 * an exit its `fast` does not handle or one after which the loop has work
 * (`fast_ends`), as Linux's vcpu_enter_guest enters again from its fast path
 * (arch/x86/kvm/x86.c:10569-10595): 0 for an exit the backend's `exit` names, 1
 * for one `fast` handled, -EAGAIN when a request or a switch came between the
 * check and the first entry, or the backend's error. The vCPU is IN_GUEST from
 * before its requests are read, so a kick that comes after the read finds it
 * there and interrupts the processor, as Linux's vcpu_enter_guest sets
 * IN_GUEST_MODE before kvm_vcpu_exit_request (x86.c:10491, 10519); it stays
 * IN_GUEST across the fast path's entries. A request the loop has not taken
 * cancels the entry (KVM_REQ_NOENTER): one that a kick set after the loop's
 * look would otherwise wait for the next exit. So does a stop or start of the
 * host's clock after the loop's `kvm_clock_prepare` (`entry_waits`), which
 * would otherwise enter with a kvmclock that keeps PVCLOCK_TSC_STABLE_BIT, and
 * so does a switch that a tick or a wake asked for after the loop's look
 * (`need_resched`), as Linux's kvm_vcpu_exit_request reads TIF_NEED_RESCHED
 * with interrupts off (x86.c:1715-1720, include/linux/entry-kvm.h:89), which
 * would otherwise wait in the guest for the next exit. An entry on another
 * processor than the last asks for the kvmclock without the master clock
 * (KVM_REQ_CLOCK_UPDATE), as Linux's kvm_arch_vcpu_load does when the vCPU
 * moved (arch/x86/kvm/x86.c:4584-4605), and so cancels this entry. The last
 * exit's account closes as handled in the kernel only once the guest ran; after
 * -EAGAIN or an error it stays open, and closes as one that ended KVM_RUN if
 * KVM_RUN returns before the next entry.
 */
static int32_t run_enter(struct kvm_vcpu *v)
{
    uint32_t flags = kirq_save();
    int32_t cpu = (int32_t)arch_cpu_id();
    uint64_t entry;
    int32_t r;

    if (v->cpu != cpu) {
        v->cpu = cpu;
        kvm_clock_moved_cpu(v);
    }
    v->mode = KVM_MODE_IN_GUEST;
    sched_mb(); /* the kick's request and mode, read in the other order */
    if (entry_waits(v) || current_proc->need_resched) {
        v->mode = KVM_MODE_OUTSIDE;
        kirq_restore(flags);
        return -EAGAIN;
    }
    v->has_run = 1;
    for (;;) {
        entry = arch_cycles();
        r = kvm_backend->enter(v);
        if (r != 0) {
            break;
        }
        kvm_exitstat_exit(v, entry, arch_cycles());
        if (!kvm_backend->fast || !kvm_backend->fast(v)) {
            break;
        }
        r = 1;
        if (fast_ends(v)) {
            break;
        }
        kvm_exitstat_fast(v);
        v->stat[KVM_VCPU_STAT_EXITS]++;
    }
    v->mode = KVM_MODE_OUTSIDE;
    kirq_restore(flags);
    return r;
}

/** Whether the thread has a signal to act on, as Linux's
 * xfer_to_guest_mode_work reads TIF_SIGPENDING (kernel/entry/kvm.c:11):
 * the flag (`sig_woken`) is read without a lock, and only when it is set
 * does `signal_pending`, which reads the handlers, run under the big
 * lock. */
static int run_signal(struct kvm_vcpu *v)
{
    int pending;

    if (!*(volatile uint32_t *)&current_proc->sig_woken) {
        return 0;
    }
    kvm_bkl_enter(v);
    pending = signal_pending();
    bkl_exit();
    return pending;
}

/**
 * Finish the instruction an answered read left under way, as Linux's
 * complete_userspace_io runs it before any event is chosen: while
 * `v->emul.resume` is set the backend's `exit` takes the instruction's
 * next step with no guest entered, and an exit the kernel takes is
 * answered here. 1 when an exit goes to the caller, with the run page
 * filled, 0 when the instruction is done, or -errno.
 */
static int32_t run_resume(struct kvm_vcpu *v)
{
    int32_t r;

    while (v->emul.resume && kvm_backend->exit) {
        r = kvm_backend->exit(v);
        if (r != 0) {
            return r;
        }
        r = run_exit(v);
        if (r != 0) {
            return r;
        }
    }
    return 0;
}

/**
 * Begin KVM_RUN: take CR8 from the run page with no local APIC in the
 * kernel, give the backend the answer to the last exit's read and finish
 * the instruction it was for, or the caller's hypercall or MSR access,
 * reporting a step over it under the caller's single step, then end at
 * once under immediate_exit, as
 * Linux's kvm_arch_vcpu_ioctl_run tests it only after
 * complete_userspace_io. 1 to enter the guest, or what KVM_RUN returns:
 * 0 with `exit_reason` set, -EINTR, or -errno.
 */
static int32_t run_begin(struct kvm_vcpu *v)
{
    struct kvm_run *run = v->run;
    int32_t r;

    if (v->vm->irqchip == KVM_IRQCHIP_NONE && run->cr8 != v->sregs.cr8) {
        if (run->cr8 > 15u) {
            return -EINVAL;
        }
        v->sregs.cr8 = run->cr8;
        v->dirty |= KVM_PART_CR8;
    }
    if (v->pending) {
        run_complete(v);
    }
    kvm_hv_complete(v);
    kvm_xen_complete(v);
    kvm_msr_complete(v);
    kvm_emul_stepped(v);
    r = run_resume(v);
    if (r != 0) {
        return r > 0 ? 0 : r;
    }
    return run->immediate_exit ? -EINTR : 1;
}

/** The `sched_out` of a vCPU's thread across KVM_RUN, as Linux's
 * kvm_sched_out (virt/kvm/kvm_main.c): `arg` is the vCPU, which a switch
 * away while its thread can still run, `preempted`, marks preempted
 * (`kvm_steal_preempted`), or runnable in Xen's runstate
 * (`kvm_xen_preempted`) while the caller set a Xen MSR, and whose
 * backend gives the host back the processor's state at every switch
 * (`put`), as kvm_arch_vcpu_put. Every switch asks for the steal time
 * once the thread runs again (KVM_REQ_STEAL_UPDATE), as Linux's
 * kvm_arch_vcpu_load does at kvm_sched_in. Under the run queues' lock
 * with interrupts off; does not sleep. */
static void vcpu_sched_out(void *arg, int preempted)
{
    struct kvm_vcpu *v = (struct kvm_vcpu *)arg;

    req_set(v, KVM_REQ_STEAL_UPDATE);
    if (preempted && kvm_xen_msr_enabled(v->vm)) {
        kvm_xen_preempted(v);
    } else if (preempted) {
        kvm_steal_preempted(v);
    }
    if (kvm_backend && kvm_backend->put) {
        kvm_backend->put(v, 0);
    }
}

struct kvm_vcpu *kvm_running_vcpu(struct kvm_vm *vm)
{
    struct kvm_vcpu *v;

    if (!current_proc || current_proc->sched_out != vcpu_sched_out) {
        return NULL;
    }
    v = (struct kvm_vcpu *)current_proc->sched_out_arg;
    return v && v->vm == vm ? v : NULL;
}

/** Whether `v` needs the run loop's work before an entry: a request, or
 * with the local APIC in the kernel a vCPU that does not run, which waits
 * in `kvm_lapic_prepare`. */
static int prepare_due(const struct kvm_vcpu *v)
{
    return (v->requests & KVM_REQ_NOENTER) ||
           (v->vm->irqchip != KVM_IRQCHIP_NONE && v->mp_state != KVM_MP_STATE_RUNNABLE);
}

/**
 * Prepare `v` for an entry, as Linux's vcpu_enter_guest does its work only
 * behind kvm_request_pending and KVM_REQ_EVENT (arch/x86/kvm/x86.c:
 * 10288-10466): the timers that are due raise KVM_REQ_TIMER
 * (`kvm_vcpu_timers`), and with no request and the vCPU running there is
 * nothing more. Otherwise each source's work runs behind its bit: a dirty
 * ring with no more than its reserve free ends the call first, as Linux's
 * kvm_dirty_ring_check_request (virt/kvm/dirty_ring.c:198-212), its bit staying
 * set while it is so; then Hyper-V's exits, the counters and their PMI,
 * the local APIC's work and its halted wait, the kvmclock with the
 * reference TSC page, the steal time, the MSR filter and the event to
 * inject. A
 * request that a step sets for an earlier one, as the steal time a halted
 * wait asks for, cancels the entry, which the next pass takes
 * (KVM_REQ_NOENTER). 1 when ready, 0 for an exit to the caller, or a
 * negative error. Without the big lock; the helpers take the locks they
 * need, and may write logged guest pages.
 */
static int32_t run_prepare(struct kvm_vcpu *v)
{
    int32_t r;

    kvm_vcpu_timers(v);
    kvm_xen_entry(v);
    if (!prepare_due(v)) {
        return 1;
    }
    if (kvm_vcpu_test_clear(v, KVM_REQ_DIRTY_RING_FULL) && kvm_dirty_ring_full(v)) {
        req_set(v, KVM_REQ_DIRTY_RING_FULL);
        v->run->exit_reason = KVM_EXIT_DIRTY_RING_FULL;
        return 0;
    }
    if (v->requests & (KVM_REQ_HV_CRASH | KVM_REQ_HV_RESET | KVM_REQ_HV_EXIT)) {
        r = kvm_hv_prepare(v);
        if (r != 0) {
            return 0;
        }
    }
    if (v->requests & (KVM_REQ_PMU | KVM_REQ_PMI)) {
        kvm_pmu_prepare(v);
    }
    if (v->vm->irqchip != KVM_IRQCHIP_NONE) {
        r = kvm_lapic_prepare(v);
        if (r != 0) {
            return r > 0 ? 0 : r;
        }
    }
    if (v->requests & (KVM_REQ_CLOCK_UPDATE | KVM_REQ_MASTERCLOCK_UPDATE)) {
        kvm_clock_prepare(v);
    }
    if (v->requests & KVM_REQ_STEAL_UPDATE) {
        kvm_steal_prepare(v);
    }
    if (kvm_vcpu_test_clear(v, KVM_REQ_MSR_FILTER) && kvm_backend->msr_filter_changed) {
        kvm_backend->msr_filter_changed(v);
    }
    if (kvm_vcpu_test_clear(v, KVM_REQ_EVENT)) {
        kvm_xen_refresh(v);
        if (kvm_backend->inject) {
            kvm_backend->inject(v);
        }
    }
    return 1;
}

/**
 * KVM_RUN's loop, as Linux's kvm_arch_vcpu_ioctl_run and vcpu_run: begin
 * with `run_begin`, then enter until an exit the caller answers, a signal,
 * or a request to leave. With the local APIC in the kernel,
 * `kvm_lapic_prepare` readies each entry, and an exit the kernel took ends
 * the call when the caller asked for an interrupt window and may inject
 * now. Before each entry the loop takes a switch the tick asked for at a
 * slice's end, or that a wake, a priority change or an affinity move asked
 * for on behalf of a real-time thread that outranks the vCPU
 * (`sched_take_preempt`), as Linux's xfer_to_guest_mode_handle_work takes
 * TIF_NEED_RESCHED, since an exit the kernel answers never reaches the
 * system call's exit, where that switch is otherwise taken: without it a
 * guest that keeps the kernel busy keeps every other thread off its
 * processor. It takes no other switch, as
 * `sched_service_point` would whenever a thread is queued: a guest exits
 * thousands of times a second, and giving the processor away at each exit
 * cost the vCPU up to a slice each time. The steal time says preempted
 * when that switch, or any other that leaves the thread able to run, goes
 * to another thread (`vcpu_sched_out`), and only for a switch at the loop's
 * top after an exit for a host interrupt: the loop clears `at_boundary`
 * right after it, as Linux clears at_instruction_boundary at the top of
 * vcpu_run's loop, since the work before the next entry may start an
 * instruction of the guest's. The switch's flag is read with interrupts
 * on, as Linux's xfer_to_guest_mode_work_pending reads TIF_NEED_RESCHED
 * (include/linux/entry-kvm.h): the tick only sets it and switches no
 * thread inside a call, so interrupts go off only for a switch to take.
 * Returns 0 with `exit_reason` set,
 * -EINTR with it KVM_EXIT_INTR for a signal or a kick, or -errno.
 */
static int32_t run_loop(struct kvm_vcpu *v)
{
    struct kvm_run *run = v->run;
    uint32_t flags;
    int32_t r;

    r = run_begin(v);
    if (r != 1) {
        return r;
    }
    for (;;) {
        kvm_exitstat_prepare(v);
        if (current_proc->need_resched) {
            flags = kirq_save();
            sched_take_preempt();
            kirq_restore(flags);
        }
        v->at_boundary = 0; /* the steps below may start an instruction */
        if (kvm_vcpu_test_clear(v, KVM_REQ_TRIPLE_FAULT)) {
            run->exit_reason = KVM_EXIT_SHUTDOWN;
            return 0;
        }
        r = kvm_vcpu_test_clear(v, KVM_REQ_EXIT);
        if (run_signal(v)) {
            v->stat[KVM_VCPU_STAT_SIGNAL_EXITS]++;
            r = 1;
        }
        if (r) {
            run->exit_reason = KVM_EXIT_INTR;
            return -EINTR;
        }
        r = run_prepare(v);
        if (r != 1) {
            return r;
        }
        r = run_enter(v);
        if (r == -EAGAIN) {
            continue;
        }
        if (r < 0) {
            return r;
        }
        v->stat[KVM_VCPU_STAT_EXITS]++;
        if (v->tsc_catchup) {
            /* As Linux's vcpu_enter_guest under tsc_always_catchup. */
            req_set(v, KVM_REQ_CLOCK_UPDATE);
        }
        if (r == 0) {
            r = kvm_backend->exit ? kvm_backend->exit(v) : -EOPNOTSUPP;
            if (r != 0) {
                return r;
            }
        }
        r = run_exit(v);
        if (r == 0) {
            /* A read a device in the kernel answered. */
            r = run_resume(v);
        }
        if (r != 0) {
            return r > 0 ? 0 : r;
        }
        if (v->vm->irqchip == KVM_IRQCHIP_SPLIT && run->request_interrupt_window &&
            kvm_lapic_ready(v)) {
            run->exit_reason = KVM_EXIT_IRQ_WINDOW_OPEN;
            return 0;
        }
    }
}

static int32_t sync_take(struct kvm_vcpu *v);
static void sync_give(struct kvm_vcpu *v);

/** KVM_RUN, whose argument is 0, without the big lock, under the signal
 * mask of KVM_SET_SIGNAL_MASK when one is set, with the old mask back
 * after, as Linux's kvm_sigset_activate and kvm_sigset_deactivate swap
 * them (virt/kvm/kvm_main.c:3668-3689); the swap takes the big lock, which
 * the signal code needs. The old mask is the thread's `sig_real_blocked`
 * meanwhile, as Linux keeps it in real_blocked, so that a signal the mask
 * lets through and the old mask blocks ends the run with -EINTR and stays
 * pending, and does not end the process at its post at the default action
 * (`sig_fatal_now`), as SIGUSR1 kicks dirty_log_test's vCPU. The
 * run page's `kvm_dirty_regs` names the register sets taken from it first
 * and `kvm_valid_regs` those written to it last (KVM_CAP_SYNC_REGS);
 * -EINVAL for a set outside KVM_SYNC_X86_VALID_FIELDS in either, or for
 * registers `sync_take` refuses. */
static int32_t vcpu_run(struct kvm_vcpu *v, uintptr_t arg)
{
    struct kvm_run *run = v->run;
    uint64_t old = 0;
    int32_t r;

    if (arg) {
        return -EINVAL;
    }
    /* Linux's vcpu_load asks for the steal time at each ioctl. */
    req_set(v, KVM_REQ_STEAL_UPDATE);
    if (v->has_sigmask) {
        kvm_bkl_enter(v);
        old = current_proc->sig_blocked;
        current_proc->sig_real_blocked = old;
        signal_set_blocked(v->sigmask);
        bkl_exit();
    }
    if ((run->kvm_valid_regs | run->kvm_dirty_regs) & ~(uint64_t)KVM_SYNC_X86_VALID_FIELDS) {
        r = -EINVAL;
    } else {
        r = sync_take(v);
        if (r == 0) {
            current_proc->sched_out_arg = v;
            current_proc->sched_out = vcpu_sched_out;
            r = run_loop(v);
            kvm_xen_run_end(v);
            vcpu_put(v, 1);
            current_proc->sched_out = NULL;
        }
    }
    if (v->has_sigmask) {
        kvm_bkl_enter(v);
        signal_set_blocked(old);
        current_proc->sig_real_blocked = 0;
        bkl_exit();
    }
    kvm_exitstat_done(v);
    sync_give(v);
    run_post(v);
    return r;
}

/** Whether `v`'s CPUID offers the feature in bit `bit` of the register
 * `reg` (0 EAX to 3 EDX) of `function`'s subleaf 0; for a NULL `v`,
 * whether KVM_GET_SUPPORTED_CPUID does. */
static int guest_has(const struct kvm_vcpu *v, uint32_t function, int reg, int bit)
{
    const struct kvm_cpuid_entry2 *e;
    uint32_t r;

    if (!v) {
        return kvm_cpuid_host_has(function, reg, bit);
    }
    e = kvm_cpuid_find(v, function, 0);
    if (!e) {
        return 0;
    }
    r = reg == 0 ? e->eax : reg == 1 ? e->ebx : reg == 2 ? e->ecx : e->edx;
    return (r >> bit) & 1u;
}

uint32_t kvm_guest_maxphyaddr(const struct kvm_vcpu *v)
{
    const struct kvm_cpuid_entry2 *e = kvm_cpuid_find(v, 0x80000000u, 0);

    if (e && e->eax >= 0x80000008u) {
        e = kvm_cpuid_find(v, 0x80000008u, 0);
        if (e) {
            return e->eax & 0xFFu;
        }
    }
    return 36;
}

/** The CR4 bits the CPUID of `v` reserves, or for a NULL `v` the CPUID
 * KVM offers, as Linux's __cr4_reserved_bits. */
static uint64_t cr4_reserved_by(const struct kvm_vcpu *v)
{
    uint64_t ok = CR4_BASE;

    if (guest_has(v, 7, 2, 2)) {
        ok |= CR4_UMIP;
    }
    if (guest_has(v, 7, 2, 16)) {
        ok |= CR4_LA57;
    }
    if (guest_has(v, 7, 1, 0)) {
        ok |= CR4_FSGSBASE;
    }
    if (guest_has(v, 1, 2, 17)) {
        ok |= CR4_PCIDE;
    }
    if (guest_has(v, 1, 2, 26)) {
        ok |= CR4_OSXSAVE;
    }
    if (guest_has(v, 7, 1, 7)) {
        ok |= CR4_SMEP;
    }
    if (guest_has(v, 7, 1, 20)) {
        ok |= CR4_SMAP;
    }
    if (guest_has(v, 7, 2, 3)) {
        ok |= CR4_PKE;
    }
    return ~ok;
}

uint64_t kvm_cr4_reserved(const struct kvm_vcpu *v)
{
    return v->cr4_reserved;
}

/** Whether `s` is a state KVM_SET_SREGS takes, as Linux's
 * kvm_is_valid_sregs, kvm_is_valid_cr0 and kvm_is_valid_cr4 judge it,
 * with the APIC base's reserved bits as kvm_set_apic_base judges them. */
static int sregs_valid(const struct kvm_vcpu *v, const struct kvm_sregs *s)
{
    uint64_t gpa_reserved = ~0ull << kvm_guest_maxphyaddr(v);
    uint64_t apic_reserved = gpa_reserved | 0x2FFu;

    if ((s->efer & EFER_LME) && (s->cr0 & CR0_PG)) {
        if (!(s->cr4 & CR4_PAE) || !(s->efer & EFER_LMA) || (s->cr3 & gpa_reserved)) {
            return 0;
        }
    } else if ((s->efer & EFER_LMA) || s->cs.l) {
        return 0;
    }
    if ((s->cr0 >> 32) != 0 || ((s->cr0 & CR0_NW) && !(s->cr0 & CR0_CD)) ||
        ((s->cr0 & CR0_PG) && !(s->cr0 & CR0_PE))) {
        return 0;
    }
    if (s->cr4 & kvm_cr4_reserved(v)) {
        return 0;
    }
    if (!guest_has(v, 1, 2, 21)) {
        apic_reserved |= APIC_BASE_X2APIC;
    }
    if ((s->apic_base & apic_reserved) ||
        ((s->apic_base & APIC_BASE_X2APIC) && !(s->apic_base & APIC_BASE_ENABLE))) {
        return 0;
    }
    return 1;
}

/** The special registers of `v` into `s`, as KVM_GET_SREGS gives them:
 * with the interrupt waiting for injection, if not a software one, set in
 * the bitmap. */
static void sregs_fill(struct kvm_vcpu *v, struct kvm_sregs *s)
{
    vcpu_save(v, KVM_PART_SREGS | KVM_PART_EVENTS);
    *s = v->sregs;
    if (v->vm->irqchip != KVM_IRQCHIP_NONE) {
        s->cr8 = kvm_lapic_cr8(v);
    }
    k_memset(s->interrupt_bitmap, 0, sizeof(s->interrupt_bitmap));
    if (v->events.interrupt.injected && !v->events.interrupt.soft) {
        s->interrupt_bitmap[v->events.interrupt.nr / 64u] |= 1ull << (v->events.interrupt.nr % 64u);
    }
}

/** Give `v` the special registers `s`, as KVM_SET_SREGS takes them, and
 * queue the first interrupt of the bitmap for injection; with the local
 * APIC in the kernel, its base and its TPR from CR8 too. 0, or -EINVAL for
 * a state Linux refuses. */
static int32_t sregs_apply(struct kvm_vcpu *v, const struct kvm_sregs *s)
{
    uint32_t i;

    if (!sregs_valid(v, s)) {
        return -EINVAL;
    }
    if (v->vm->irqchip != KVM_IRQCHIP_NONE) {
        kvm_lapic_set_base(v, s->apic_base, 1);
        kvm_lapic_set_cr8(v, s->cr8);
    }
    v->sregs = *s;
    k_memset(v->sregs.interrupt_bitmap, 0, sizeof(v->sregs.interrupt_bitmap));
    v->dirty |= KVM_PART_SREGS;
    /* `inject` runs before the next entry, where a backend reads PAE's
     * PDPTEs from guest memory, as Linux's __set_sregs loads them. */
    req_set(v, KVM_REQ_EVENT);
    for (i = 0; i < KVM_NR_INTERRUPTS; i++) {
        if ((s->interrupt_bitmap[i / 64u] >> (i % 64u)) & 1u) {
            v->events.interrupt.injected = 1;
            v->events.interrupt.nr = (uint8_t)i;
            v->events.interrupt.soft = 0;
            v->dirty |= KVM_PART_EVENTS;
            req_set(v, KVM_REQ_EVENT);
            break;
        }
    }
    return 0;
}

/** KVM_GET_SREGS. */
static int32_t get_sregs(struct kvm_vcpu *v, uintptr_t arg)
{
    struct kvm_sregs s;

    sregs_fill(v, &s);
    return copy_to_user((void __user *)arg, &s, sizeof(s)) ? -EFAULT : 0;
}

/** KVM_SET_REGS: 0, or -EFAULT with the registers as they were, since the
 * copy goes to a local first, as Linux's kvm_vcpu_ioctl stages them
 * (virt/kvm/kvm_main.c:4540). */
static int32_t set_regs(struct kvm_vcpu *v, uintptr_t arg)
{
    struct kvm_regs regs;

    if (copy_from_user(&regs, (const void __user *)arg, sizeof(regs)) != 0) {
        return -EFAULT;
    }
    v->regs = regs;
    v->dirty |= KVM_PART_REGS;
    req_set(v, KVM_REQ_EVENT);
    return 0;
}

/** KVM_SET_SREGS: 0, -EINVAL for a state Linux refuses, or -EFAULT. */
static int32_t set_sregs(struct kvm_vcpu *v, uintptr_t arg)
{
    struct kvm_sregs s;

    if (copy_from_user(&s, (const void __user *)arg, sizeof(s)) != 0) {
        return -EFAULT;
    }
    return sregs_apply(v, &s);
}

/** KVM_GET_FPU: the legacy region of the XSAVE image, without MXCSR, which
 * Linux's kvm_arch_vcpu_ioctl_get_fpu leaves out too. */
static int32_t get_fpu(struct kvm_vcpu *v, uintptr_t arg)
{
    struct kvm_fpu f;
    const uint8_t *ex = v->xsave;

    vcpu_save(v, KVM_PART_FPU);
    k_memset(&f, 0, sizeof(f));
    k_memcpy(f.fpr, ex + 32, sizeof(f.fpr));
    k_memcpy(&f.fcw, ex + 0, 2);
    k_memcpy(&f.fsw, ex + 2, 2);
    f.ftwx = ex[4];
    k_memcpy(&f.last_opcode, ex + 6, 2);
    k_memcpy(&f.last_ip, ex + 8, 8);
    k_memcpy(&f.last_dp, ex + 16, 8);
    k_memcpy(f.xmm, ex + 160, sizeof(f.xmm));
    return copy_to_user((void __user *)arg, &f, sizeof(f)) ? -EFAULT : 0;
}

/** KVM_SET_FPU: the same fields into the legacy region, with x87 and SSE
 * then marked in use so the next entry loads them. */
static int32_t set_fpu(struct kvm_vcpu *v, uintptr_t arg)
{
    struct kvm_fpu f;
    uint8_t *ex = v->xsave;
    uint64_t bv;

    if (copy_from_user(&f, (const void __user *)arg, sizeof(f)) != 0) {
        return -EFAULT;
    }
    vcpu_save(v, KVM_PART_FPU);
    k_memcpy(ex + 32, f.fpr, sizeof(f.fpr));
    k_memcpy(ex + 0, &f.fcw, 2);
    k_memcpy(ex + 2, &f.fsw, 2);
    ex[4] = f.ftwx;
    k_memcpy(ex + 6, &f.last_opcode, 2);
    k_memcpy(ex + 8, &f.last_ip, 8);
    k_memcpy(ex + 16, &f.last_dp, 8);
    k_memcpy(ex + 160, f.xmm, sizeof(f.xmm));
    k_memcpy(&bv, ex + XSAVE_HDR, sizeof(bv));
    bv |= XFEATURE_X87 | XFEATURE_SSE;
    k_memcpy(ex + XSAVE_HDR, &bv, sizeof(bv));
    v->dirty |= KVM_PART_FPU;
    return 0;
}

/**
 * KVM_GET_XSAVE and KVM_GET_XSAVE2: the XSAVE image in its standard form,
 * KVM_XSAVE_SIZE bytes, with x87, SSE and the components the guest's CPUID
 * allows marked in use, as Linux's kvm_vcpu_ioctl_x86_get_xsave2 marks
 * them: a component this host has and the guest does not would make
 * KVM_SET_XSAVE fail on a host without it. Without XSAVE the image is
 * FXSAVE's, with x87 and SSE marked. 0, -ENOMEM or -EFAULT.
 */
static int32_t get_xsave(struct kvm_vcpu *v, uintptr_t arg)
{
    uint64_t bv;
    int32_t r;
    uint8_t *ex;

    vcpu_save(v, KVM_PART_FPU);
    ex = kmalloc(KVM_XSAVE_SIZE);
    if (!ex) {
        return -ENOMEM;
    }
    k_memcpy(ex, v->xsave, KVM_XSAVE_SIZE);
    k_memcpy(&bv, ex + XSAVE_HDR, sizeof(bv));
    if (kvm_xcr0_supported() != 0) {
        bv &= kvm_xcr0_guest(v) | XFEATURE_SSE;
    } else {
        k_memset(ex + 512, 0, KVM_XSAVE_SIZE - 512);
        bv = XFEATURE_X87 | XFEATURE_SSE;
    }
    k_memcpy(ex + XSAVE_HDR, &bv, sizeof(bv));
    r = copy_to_user((void __user *)arg, ex, KVM_XSAVE_SIZE) ? -EFAULT : 0;
    kfree(ex);
    return r;
}

/**
 * KVM_SET_XSAVE: an XSAVE image in its standard form, which Linux's
 * fpu_copy_uabi_to_guest_fpstate takes when its header marks only
 * components KVM supports on this host, whatever the guest's CPUID allows,
 * its compaction word and reserved bytes are zero, and, with x87, SSE or
 * AVX marked, its MXCSR sets no reserved bit. The check is against the
 * host's components so that a vCPU's state loads into one not yet given
 * its CPUID. With none of those three marked the vCPU keeps its MXCSR, as
 * Linux's copy_uabi_to_xstate keeps it, since XRSTOR loads MXCSR whenever
 * SSE or AVX is asked for. Without XSAVE only x87 and SSE may be marked,
 * and the 512 bytes of FXSAVE's image are taken. 0, -EINVAL, -ENOMEM or
 * -EFAULT.
 */
static int32_t set_xsave(struct kvm_vcpu *v, uintptr_t arg)
{
    const uint64_t legacy = XFEATURE_X87 | XFEATURE_SSE | XFEATURE_YMM;
    uint64_t bv, comp, xcr0 = kvm_xcr0_supported();
    uint32_t mxcsr, i;
    int32_t r = -EINVAL;
    uint8_t *ex;

    ex = kmalloc(KVM_XSAVE_SIZE);
    if (!ex) {
        return -ENOMEM;
    }
    if (copy_from_user(ex, (const void __user *)arg, KVM_XSAVE_SIZE) != 0) {
        r = -EFAULT;
        goto out;
    }
    k_memcpy(&bv, ex + XSAVE_HDR, sizeof(bv));
    k_memcpy(&comp, ex + XSAVE_HDR + 8, sizeof(comp));
    k_memcpy(&mxcsr, ex + 24, sizeof(mxcsr));
    if (xcr0 == 0) {
        if ((bv & ~(XFEATURE_X87 | XFEATURE_SSE)) || (mxcsr & ~MXCSR_MASK)) {
            goto out;
        }
        vcpu_save(v, KVM_PART_FPU);
        k_memcpy(v->xsave, ex, 512);
        v->dirty |= KVM_PART_FPU;
        r = 0;
        goto out;
    }
    if ((bv & ~xcr0) || comp != 0) {
        goto out;
    }
    for (i = XSAVE_HDR + 16; i < XSAVE_HDR + 64; i++) {
        if (ex[i] != 0) {
            goto out;
        }
    }
    if ((bv & legacy) && (mxcsr & ~MXCSR_MASK)) {
        goto out;
    }
    if (!(bv & legacy)) {
        vcpu_save(v, KVM_PART_FPU);
        k_memcpy(ex + 24, v->xsave + 24, 8); /* MXCSR and MXCSR_MASK */
    }
    k_memcpy(v->xsave, ex, KVM_XSAVE_SIZE);
    v->dirty |= KVM_PART_FPU;
    r = 0;
out:
    kfree(ex);
    return r;
}

/** Whether `xcr0` is a value XSETBV of XCR0 takes, as Linux's __kvm_set_xcr
 * judges it for `v`: x87 set, AVX with SSE, AVX-512's three together and
 * with AVX, and nothing the guest's CPUID does not allow. */
static int xcr0_valid(const struct kvm_vcpu *v, uint64_t xcr0)
{
    if (!(xcr0 & XFEATURE_X87)) {
        return 0;
    }
    if ((xcr0 & XFEATURE_YMM) && !(xcr0 & XFEATURE_SSE)) {
        return 0;
    }
    if (xcr0 & ~(kvm_xcr0_guest(v) | XFEATURE_X87)) {
        return 0;
    }
    if (xcr0 & XFEATURE_AVX512) {
        if ((xcr0 & XFEATURE_AVX512) != XFEATURE_AVX512 || !(xcr0 & XFEATURE_YMM)) {
            return 0;
        }
    }
    return 1;
}

/** KVM_GET_XCRS: XCR0 alone, the one Linux gives, or none without XSAVE. */
static int32_t get_xcrs(struct kvm_vcpu *v, uintptr_t arg)
{
    struct kvm_xcrs ex;

    vcpu_save(v, KVM_PART_FPU);
    k_memset(&ex, 0, sizeof(ex));
    if (kvm_xcr0_supported() != 0) {
        ex.nr_xcrs = 1;
        ex.xcrs[0].xcr = 0;
        ex.xcrs[0].value = v->xcr0;
    }
    return copy_to_user((void __user *)arg, &ex, sizeof(ex)) ? -EFAULT : 0;
}

/** KVM_SET_XCRS: the first entry for XCR0 sets it, and Linux ignores the
 * rest. 0, -EINVAL without XSAVE, for flags, too many entries or a value
 * XSETBV refuses, or -EFAULT. */
static int32_t set_xcrs(struct kvm_vcpu *v, uintptr_t arg)
{
    struct kvm_xcrs ex;
    uint32_t i;

    if (copy_from_user(&ex, (const void __user *)arg, sizeof(ex)) != 0) {
        return -EFAULT;
    }
    if (kvm_xcr0_supported() == 0 || ex.nr_xcrs > KVM_MAX_XCRS || ex.flags) {
        return -EINVAL;
    }
    for (i = 0; i < ex.nr_xcrs; i++) {
        if (ex.xcrs[i].xcr == 0) {
            if (!xcr0_valid(v, ex.xcrs[i].value)) {
                return -EINVAL;
            }
            v->xcr0 = ex.xcrs[i].value;
            kvm_cpuid_xsave_sizes(v, v->cpuid, v->cpuid_nent);
            v->dirty |= KVM_PART_FPU;
            break;
        }
    }
    return 0;
}

/**
 * KVM_GET_MSRS and KVM_SET_MSRS, `set` naming the second: the entries
 * after the header moved in order until one fails. The count moved,
 * -E2BIG for KVM_MAX_MSR_ENTRIES or more, -ENOMEM or -EFAULT.
 */
static int32_t msrs_io(struct kvm_vcpu *v, uintptr_t arg, int set)
{
    struct kvm_msrs head;
    struct kvm_msr_entry *ent;
    uint32_t i, bytes;
    int32_t r = -EFAULT;

    if (copy_from_user(&head, (const void __user *)arg, sizeof(head)) != 0) {
        return -EFAULT;
    }
    if (head.nmsrs >= KVM_MAX_MSR_ENTRIES) {
        return -E2BIG;
    }
    bytes = head.nmsrs * (uint32_t)sizeof(*ent);
    ent = kmalloc(bytes + 1u);
    if (!ent) {
        return -ENOMEM;
    }
    if (copy_from_user(ent, (const void __user *)(arg + sizeof(head)), bytes) != 0) {
        goto out;
    }
    /* A set too, since the APIC base it may write is part of the special
     * registers the backend loads whole. */
    vcpu_save(v, KVM_PART_MSRS | KVM_PART_TSC | KVM_PART_SREGS);
    for (i = 0; i < head.nmsrs; i++) {
        if (set ? kvm_msr_set(v, ent[i].index, ent[i].data, 1) != 0
                : kvm_msr_get(v, ent[i].index, &ent[i].data, 1) != 0) {
            break;
        }
    }
    if (!set && copy_to_user((void __user *)(arg + sizeof(head)), ent, bytes) != 0) {
        goto out;
    }
    r = (int32_t)i;
out:
    kfree(ent);
    return r;
}

/** Whether the `n` bytes at `a` and `b` are the same. */
static int bytes_equal(const void *a, const void *b, uint32_t n)
{
    const uint8_t *p = (const uint8_t *)a, *q = (const uint8_t *)b;
    uint32_t i;

    for (i = 0; i < n; i++) {
        if (p[i] != q[i]) {
            return 0;
        }
    }
    return 1;
}

/**
 * KVM_SET_CPUID2: the guest's CPUID, which once the vCPU has run Linux
 * takes only unchanged, comparing it with leaf 0xD's sizes set for the
 * vCPU's XCR0 as it keeps its own. 0, -E2BIG past KVM_MAX_CPUID_ENTRIES,
 * -EINVAL for a change after a run, -ENOMEM or -EFAULT.
 */
static int32_t set_cpuid(struct kvm_vcpu *v, uintptr_t arg)
{
    struct kvm_cpuid2 head;
    struct kvm_cpuid_entry2 *ent = NULL;
    uint32_t bytes;

    if (copy_from_user(&head, (const void __user *)arg, sizeof(head)) != 0) {
        return -EFAULT;
    }
    if (head.nent > KVM_MAX_CPUID_ENTRIES) {
        return -E2BIG;
    }
    bytes = head.nent * (uint32_t)sizeof(*ent);
    if (head.nent) {
        ent = kmalloc(bytes);
        if (!ent) {
            return -ENOMEM;
        }
        if (copy_from_user(ent, (const void __user *)(arg + sizeof(head)), bytes) != 0) {
            kfree(ent);
            return -EFAULT;
        }
    }
    kvm_cpuid_xsave_sizes(v, ent, head.nent);
    if (v->has_run) {
        int same = head.nent == v->cpuid_nent && bytes_equal(ent, v->cpuid, bytes);
        kfree(ent);
        return same ? 0 : -EINVAL;
    }
    if (kvm_hv_set_cpuid(v, ent, head.nent) != 0) {
        kfree(ent);
        return -ENOMEM;
    }
    kfree(v->cpuid);
    v->cpuid = ent;
    v->cpuid_nent = head.nent;
    v->cpuid_set = 1;
    v->cr4_reserved = cr4_reserved_by(NULL) | cr4_reserved_by(v);
    kvm_pv_cpuid_update(v, 1);
    kvm_pmu_refresh(v);
    v->dirty |= KVM_PART_CPUID;
    return 0;
}

/** KVM_GET_CPUID2: the entries KVM_SET_CPUID2 gave, and their count, when
 * the caller's room holds them; -E2BIG when it does not, or -EFAULT. */
static int32_t get_cpuid(struct kvm_vcpu *v, uintptr_t arg)
{
    struct kvm_cpuid2 head;

    if (copy_from_user(&head, (const void __user *)arg, sizeof(head)) != 0) {
        return -EFAULT;
    }
    if (head.nent < v->cpuid_nent) {
        return -E2BIG;
    }
    if (v->cpuid_nent && copy_to_user((void __user *)(arg + sizeof(head)), v->cpuid,
                                      v->cpuid_nent * sizeof(struct kvm_cpuid_entry2)) != 0) {
        return -EFAULT;
    }
    head.nent = v->cpuid_nent;
    return copy_to_user((void __user *)arg, &head, sizeof(head)) ? -EFAULT : 0;
}

/** The events waiting in `v` into `ev`, as KVM_GET_VCPU_EVENTS gives
 * them, with an exception reported as injected, since the caller has not
 * enabled exception payloads. */
static void events_fill(struct kvm_vcpu *v, struct kvm_vcpu_events *ev)
{
    vcpu_save(v, KVM_PART_EVENTS);
    *ev = v->events;
    ev->exception.injected = (uint8_t)(ev->exception.injected || ev->exception.pending);
    ev->exception.pending = 0;
    ev->exception_has_payload = 0;
    ev->exception_payload = 0;
    ev->nmi.pending = (uint8_t)(v->nmi_queued != 0);
    ev->flags =
        KVM_VCPUEVENT_VALID_NMI_PENDING | KVM_VCPUEVENT_VALID_SHADOW | KVM_VCPUEVENT_VALID_SMM;
    k_memset(&ev->smi, 0, sizeof(ev->smi));
    k_memset(&ev->triple_fault, 0, sizeof(ev->triple_fault));
    k_memset(ev->reserved, 0, sizeof(ev->reserved));
}

/** KVM_GET_VCPU_EVENTS. */
static int32_t get_events(struct kvm_vcpu *v, uintptr_t arg)
{
    struct kvm_vcpu_events ev;

    events_fill(v, &ev);
    return copy_to_user((void __user *)arg, &ev, sizeof(ev)) ? -EFAULT : 0;
}

/**
 * Give `v` the events `ev`, as Linux's kvm_vcpu_ioctl_x86_set_vcpu_events
 * takes them with neither exception payloads nor triple-fault events
 * enabled and no SMM: the exception, the interrupt and the NMI's state
 * always, the NMI's pending count, the SIPI vector and the interrupt
 * shadow when their flags say so. 0, or -EINVAL for another flag, a
 * payload, an exception past 31 or the NMI's vector, or SMM.
 */
static int32_t events_apply(struct kvm_vcpu *v, const struct kvm_vcpu_events *ev)
{
    if (ev->flags & ~(KVM_VCPUEVENT_VALID_NMI_PENDING | KVM_VCPUEVENT_VALID_SIPI_VECTOR |
                      KVM_VCPUEVENT_VALID_SHADOW | KVM_VCPUEVENT_VALID_SMM)) {
        return -EINVAL;
    }
    if (ev->exception.injected && (ev->exception.nr > 31 || ev->exception.nr == NMI_VECTOR)) {
        return -EINVAL;
    }
    if ((ev->flags & KVM_VCPUEVENT_VALID_SMM) &&
        (ev->smi.smm || ev->smi.pending || ev->smi.smm_inside_nmi)) {
        return -EINVAL;
    }
    vcpu_save(v, KVM_PART_EVENTS);
    v->events.exception = ev->exception;
    v->events.exception.pending = 0;
    v->events.interrupt.injected = ev->interrupt.injected;
    v->events.interrupt.nr = ev->interrupt.nr;
    v->events.interrupt.soft = ev->interrupt.soft;
    if (ev->flags & KVM_VCPUEVENT_VALID_SHADOW) {
        v->events.interrupt.shadow = ev->interrupt.shadow;
    }
    v->events.nmi.injected = ev->nmi.injected;
    v->events.nmi.masked = ev->nmi.masked;
    if (ev->flags & KVM_VCPUEVENT_VALID_NMI_PENDING) {
        v->nmi_queued = ev->nmi.pending ? 1u : 0u;
    }
    if (ev->flags & KVM_VCPUEVENT_VALID_SIPI_VECTOR) {
        v->events.sipi_vector = ev->sipi_vector;
    }
    v->dirty |= KVM_PART_EVENTS;
    req_set(v, KVM_REQ_EVENT);
    return 0;
}

/**
 * Take from the run page the register sets its `kvm_dirty_regs` names, as
 * KVM_SET_REGS, KVM_SET_SREGS and KVM_SET_VCPU_EVENTS take them, and clear
 * each set's bit, as Linux's sync_regs. The special registers and the
 * events are judged in a copy, since the page is the caller's to write
 * while this reads it. 0, or -EINVAL for a set Linux refuses, with its bit
 * left.
 */
static int32_t sync_take(struct kvm_vcpu *v)
{
    struct kvm_run *run = v->run;
    struct kvm_sregs sregs;
    struct kvm_vcpu_events events;

    if (run->kvm_dirty_regs & KVM_SYNC_X86_REGS) {
        v->regs = run->s.regs.regs;
        v->dirty |= KVM_PART_REGS;
        req_set(v, KVM_REQ_EVENT);
        run->kvm_dirty_regs &= ~(uint64_t)KVM_SYNC_X86_REGS;
    }
    if (run->kvm_dirty_regs & KVM_SYNC_X86_SREGS) {
        sregs = run->s.regs.sregs;
        if (sregs_apply(v, &sregs) != 0) {
            return -EINVAL;
        }
        run->kvm_dirty_regs &= ~(uint64_t)KVM_SYNC_X86_SREGS;
    }
    if (run->kvm_dirty_regs & KVM_SYNC_X86_EVENTS) {
        events = run->s.regs.events;
        if (events_apply(v, &events) != 0) {
            return -EINVAL;
        }
        run->kvm_dirty_regs &= ~(uint64_t)KVM_SYNC_X86_EVENTS;
    }
    return 0;
}

/** Write to the run page the register sets its `kvm_valid_regs` names, as
 * KVM_GET_REGS, KVM_GET_SREGS and KVM_GET_VCPU_EVENTS give them, as
 * Linux's store_regs does at every return of KVM_RUN. */
static void sync_give(struct kvm_vcpu *v)
{
    struct kvm_run *run = v->run;
    uint64_t valid = run->kvm_valid_regs;
    struct kvm_sregs sregs;
    struct kvm_vcpu_events events;

    if (valid & KVM_SYNC_X86_REGS) {
        vcpu_save(v, KVM_PART_REGS);
        run->s.regs.regs = v->regs;
    }
    if (valid & KVM_SYNC_X86_SREGS) {
        sregs_fill(v, &sregs);
        run->s.regs.sregs = sregs;
    }
    if (valid & KVM_SYNC_X86_EVENTS) {
        events_fill(v, &events);
        run->s.regs.events = events;
    }
}

/** KVM_SET_VCPU_EVENTS: 0, `events_apply`'s -EINVAL, or -EFAULT. */
static int32_t set_events(struct kvm_vcpu *v, uintptr_t arg)
{
    struct kvm_vcpu_events ev;

    if (copy_from_user(&ev, (const void __user *)arg, sizeof(ev)) != 0) {
        return -EFAULT;
    }
    return events_apply(v, &ev);
}

/** KVM_GET_DEBUGREGS. */
static int32_t get_debugregs(struct kvm_vcpu *v, uintptr_t arg)
{
    struct kvm_debugregs d;

    vcpu_save(v, KVM_PART_DEBUG);
    k_memset(&d, 0, sizeof(d));
    k_memcpy(d.db, v->debugregs.db, sizeof(d.db));
    d.dr6 = v->debugregs.dr6;
    d.dr7 = v->debugregs.dr7;
    return copy_to_user((void __user *)arg, &d, sizeof(d)) ? -EFAULT : 0;
}

/** KVM_SET_DEBUGREGS: -EINVAL for flags, or for DR6 or DR7 with a bit set
 * above bit 31, as Linux's kvm_dr6_valid and kvm_dr7_valid refuse. */
static int32_t set_debugregs(struct kvm_vcpu *v, uintptr_t arg)
{
    struct kvm_debugregs d;

    if (copy_from_user(&d, (const void __user *)arg, sizeof(d)) != 0) {
        return -EFAULT;
    }
    if (d.flags || (d.dr6 >> 32) || (d.dr7 >> 32)) {
        return -EINVAL;
    }
    k_memcpy(v->debugregs.db, d.db, sizeof(d.db));
    v->debugregs.dr6 = d.dr6;
    v->debugregs.dr7 = d.dr7;
    v->dirty |= KVM_PART_DEBUG;
    return 0;
}

/** KVM_SET_GUEST_DEBUG, which the backend acts on at the next entry,
 * where events are weighed again: an INJECT flag queues #DB or #BP,
 * -EBUSY while an exception waits; -EINVAL for a flag Linux does not
 * know. */
static int32_t set_guest_debug(struct kvm_vcpu *v, uintptr_t arg)
{
    struct kvm_guest_debug d;

    if (copy_from_user(&d, (const void __user *)arg, sizeof(d)) != 0) {
        return -EFAULT;
    }
    if (d.control & ~KVM_GUESTDBG_VALID_MASK) {
        return -EINVAL;
    }
    if (d.control & (KVM_GUESTDBG_INJECT_DB | KVM_GUESTDBG_INJECT_BP)) {
        vcpu_save(v, KVM_PART_EVENTS);
        if (v->events.exception.injected || v->events.exception.pending) {
            return -EBUSY;
        }
        queue_exception(v, (d.control & KVM_GUESTDBG_INJECT_DB) ? DB_VECTOR : BP_VECTOR);
    }
    if (!(d.control & KVM_GUESTDBG_ENABLE)) {
        d.control = 0;
    }
    v->guest_debug = d;
    v->dirty |= KVM_PART_DEBUG;
    /* An event BLOCKIRQ held goes in now, as Linux's kvm_set_rflags asks. */
    req_set(v, KVM_REQ_EVENT);
    return 0;
}

/** KVM_INTERRUPT: with no interrupt controller in the kernel, queue the
 * vector for injection, -ENXIO while one waits; with the split controller,
 * the local APIC's (vlapic.c). -EINVAL for a vector past 255. */
static int32_t interrupt(struct kvm_vcpu *v, uintptr_t arg)
{
    struct kvm_interrupt irq;

    if (copy_from_user(&irq, (const void __user *)arg, sizeof(irq)) != 0) {
        return -EFAULT;
    }
    if (irq.irq >= KVM_NR_INTERRUPTS) {
        return -EINVAL;
    }
    if (v->vm->irqchip != KVM_IRQCHIP_NONE) {
        return kvm_lapic_ioctl(v, KVM_INTERRUPT, arg);
    }
    vcpu_save(v, KVM_PART_EVENTS);
    if (v->events.interrupt.injected) {
        return -ENXIO;
    }
    v->events.interrupt.injected = 1;
    v->events.interrupt.nr = (uint8_t)irq.irq;
    v->events.interrupt.soft = 0;
    v->dirty |= KVM_PART_EVENTS;
    req_set(v, KVM_REQ_EVENT);
    return 0;
}

/** KVM_SET_MP_STATE: with no local APIC in the kernel, Linux takes only
 * RUNNABLE. -EINVAL for any other state then, or one Linux does not know. */
static int32_t set_mp_state(struct kvm_vcpu *v, uintptr_t arg)
{
    struct kvm_mp_state mp;

    if (copy_from_user(&mp, (const void __user *)arg, sizeof(mp)) != 0) {
        return -EFAULT;
    }
    if (mp.mp_state > KVM_MP_STATE_MAX) {
        return -EINVAL;
    }
    if (v->vm->irqchip == KVM_IRQCHIP_NONE && mp.mp_state != KVM_MP_STATE_RUNNABLE) {
        return -EINVAL;
    }
    v->mp_state = mp.mp_state;
    v->dirty |= KVM_PART_MP;
    return 0;
}

/**
 * KVM_X86_SETUP_MCE: the guest's IA32_MCG_CAP, as Linux's
 * kvm_vcpu_ioctl_x86_setup_mce takes it: 1 to KVM_MAX_MCE_BANKS banks and
 * no capability but those offered and the extended count; MCG_CTL and
 * each bank's CTL then read as all ones. 0, -EINVAL or -EFAULT.
 */
static int32_t setup_mce(struct kvm_vcpu *v, uintptr_t arg)
{
    uint64_t cap;
    uint32_t banks, i;

    if (copy_from_user(&cap, (const void __user *)arg, sizeof(cap)) != 0) {
        return -EFAULT;
    }
    banks = (uint32_t)(cap & 0xFFu);
    if (banks == 0 || banks > KVM_MAX_MCE_BANKS ||
        (cap & ~(MCG_CTL_P | MCG_SER_P | 0xFFull | 0xFF0000ull))) {
        return -EINVAL;
    }
    v->mcg_cap = cap;
    if (cap & MCG_CTL_P) {
        v->mcg_ctl = ~0ull;
    }
    for (i = 0; i < banks; i++) {
        v->mce_banks[i * 4u] = ~0ull;
    }
    v->dirty |= KVM_PART_MSRS;
    return 0;
}

/**
 * KVM_X86_SET_MCE: a machine check reported in a bank, as Linux's
 * kvm_vcpu_ioctl_x86_set_mce reports it: an uncorrected error raises #MC
 * unless reporting is off, or ends the guest in a triple fault while one
 * is in progress or CR4.MCE is clear; a corrected one is logged, with
 * OVER where it finds a valid one. 0, -EINVAL for a bank past the guest's
 * or a status not valid, or -EFAULT.
 */
static int32_t set_mce(struct kvm_vcpu *v, uintptr_t arg)
{
    struct kvm_x86_mce m;
    uint32_t banks = (uint32_t)(v->mcg_cap & 0xFFu);
    uint64_t *b;

    if (copy_from_user(&m, (const void __user *)arg, sizeof(m)) != 0) {
        return -EFAULT;
    }
    if (m.bank >= banks || !(m.status & MCI_STATUS_VAL)) {
        return -EINVAL;
    }
    b = &v->mce_banks[m.bank * 4u];
    v->dirty |= KVM_PART_MSRS;
    if (m.status & MCI_STATUS_UC) {
        if (((v->mcg_cap & MCG_CTL_P) && v->mcg_ctl != ~0ull) || b[0] != ~0ull) {
            return 0;
        }
        vcpu_save(v, KVM_PART_SREGS);
        if ((v->mcg_status & MCG_STATUS_MCIP) || !(v->sregs.cr4 & CR4_MCE)) {
            req_set(v, KVM_REQ_TRIPLE_FAULT);
            return 0;
        }
        if (b[1] & MCI_STATUS_VAL) {
            m.status |= MCI_STATUS_OVER;
        }
        b[2] = m.addr;
        b[3] = m.misc;
        v->mcg_status = m.mcg_status;
        b[1] = m.status;
        queue_exception(v, MC_VECTOR);
    } else if (!(b[1] & MCI_STATUS_VAL) || !(b[1] & MCI_STATUS_UC)) {
        if (b[1] & MCI_STATUS_VAL) {
            m.status |= MCI_STATUS_OVER;
        }
        b[2] = m.addr;
        b[3] = m.misc;
        b[1] = m.status;
    } else {
        b[1] |= MCI_STATUS_OVER;
    }
    return 0;
}

/** KVM_SET_SIGNAL_MASK: the mask KVM_RUN runs under, less SIGKILL and
 * SIGSTOP, or none for a NULL argument. -EINVAL for a length other than 8,
 * or -EFAULT. */
static int32_t set_signal_mask(struct kvm_vcpu *v, uintptr_t arg)
{
    uint32_t len;
    uint64_t mask;

    if (!arg) {
        v->has_sigmask = 0;
        return 0;
    }
    if (copy_from_user(&len, (const void __user *)arg, sizeof(len)) != 0) {
        return -EFAULT;
    }
    if (len != sizeof(mask)) {
        return -EINVAL;
    }
    if (copy_from_user(&mask, (const void __user *)(arg + sizeof(len)), sizeof(mask)) != 0) {
        return -EFAULT;
    }
    v->sigmask = mask & ~(SIGKILL_BIT | SIGSTOP_BIT);
    v->has_sigmask = 1;
    return 0;
}

/** Disable the optional VAPIC page, which QEMU does on APIC reset even
 * without KVM_CAP_VAPIC. A nonzero page is not offered by this interface. */
static int32_t set_vapic(struct kvm_vcpu *v, uintptr_t arg)
{
    uint64_t address;

    if (v->vm->irqchip == KVM_IRQCHIP_NONE) {
        return -EINVAL;
    }
    if (copy_from_user(&address, (const void __user *)arg, sizeof(address)) != 0) {
        return -EFAULT;
    }
    return address ? -EINVAL : 0;
}

/** KVM_ENABLE_CAP of a vCPU: Linux's x86 vCPU capabilities are Hyper-V's,
 * hyperv.c's, and KVM_CAP_ENFORCE_PV_FEATURE_CPUID, which holds the guest
 * to the paravirtual features its CPUID names. -EINVAL for a flag or a
 * capability not enabled, as Linux. */
static int32_t vcpu_enable_cap(struct kvm_vcpu *v, uintptr_t arg)
{
    struct kvm_enable_cap cap;

    if (copy_from_user(&cap, (const void __user *)arg, sizeof(cap)) != 0) {
        return -EFAULT;
    }
    if (cap.flags) {
        return -EINVAL;
    }
    if (cap.cap == KVM_CAP_ENFORCE_PV_FEATURE_CPUID) {
        v->pv_enforce = cap.args[0] != 0;
        if (v->pv_enforce) {
            kvm_pv_cpuid_update(v, 0);
        }
        return 0;
    }
    return kvm_hv_enable_cap(v, &cap);
}

/** KVM_SET_TSC_KHZ of a vCPU, as Linux's: 0, or -EINVAL for a rate
 * `kvm_tsc_khz_arg` or `kvm_tsc_set_khz` refuses. Before the next entry
 * the kvmclock and the reference TSC page, which follows the first vCPU's
 * TSC, are written again at the new rate, and `timer_ns` is found again,
 * since a TSC deadline armed at the old rate may now be past; a refused
 * rate changes the TSC too. */
static int32_t set_tsc_khz(struct kvm_vcpu *v, uintptr_t arg)
{
    uint32_t khz = kvm_tsc_khz_arg(arg);
    int r;

    if (khz == 0) {
        return -EINVAL;
    }
    r = kvm_tsc_set_khz(v, khz); /* a refusal leaves the host's rate */
    v->vm->hv_clock_moved = 1;
    kvm_vcpu_kick(v, KVM_REQ_CLOCK_UPDATE | KVM_REQ_TIMER);
    return r != 0 ? -EINVAL : 0;
}

/** KVM_PRE_FAULT_MEMORY: the range at `arg` is mapped and written back
 * with what is left of it, as Linux writes it back whatever the result.
 * `kvm_tdp_pre_fault`'s result, or -EFAULT. */
static int32_t pre_fault(struct kvm_vcpu *v, uintptr_t arg)
{
    struct kvm_pre_fault_memory range;
    int32_t r;

    if (copy_from_user(&range, (const void __user *)arg, sizeof(range)) != 0) {
        return -EFAULT;
    }
    r = kvm_tdp_pre_fault(v->vm, &range);
    if (copy_to_user((void __user *)arg, &range, sizeof(range)) != 0) {
        return -EFAULT;
    }
    return r;
}

/**
 * KVM_HAS_DEVICE_ATTR, KVM_GET_DEVICE_ATTR and KVM_SET_DEVICE_ATTR of `v`,
 * by `request`, as Linux's kvm_vcpu_ioctl_device_attr: group
 * KVM_VCPU_TSC_CTRL has KVM_VCPU_TSC_OFFSET, the u64 at the attribute's
 * `addr`, which a get reads as the vCPU's TSC offset and a set gives it
 * (`kvm_tsc_offset_set`). 0, -ENXIO for another group or attribute, or
 * -EFAULT; -EINVAL for any other request, which `vcpu_state_ioctl` passes
 * on, as Linux's vCPU ioctl answers one it does not have.
 */
static int32_t device_attr(struct kvm_vcpu *v, uint32_t request, uintptr_t arg)
{
    struct kvm_device_attr attr;
    void __user *addr;
    uint64_t value;

    if (request != KVM_HAS_DEVICE_ATTR && request != KVM_GET_DEVICE_ATTR &&
        request != KVM_SET_DEVICE_ATTR) {
        return -EINVAL;
    }
    if (copy_from_user(&attr, (const void __user *)arg, sizeof(attr)) != 0) {
        return -EFAULT;
    }
    if (attr.group != KVM_VCPU_TSC_CTRL || attr.attr != KVM_VCPU_TSC_OFFSET) {
        return -ENXIO;
    }
    addr = (void __user *)(uintptr_t)attr.addr;
    if (request == KVM_GET_DEVICE_ATTR) {
        value = (uint64_t)v->tsc_offset;
        return copy_to_user(addr, &value, sizeof(value)) ? -EFAULT : 0;
    }
    if (request == KVM_SET_DEVICE_ATTR) {
        if (copy_from_user(&value, addr, sizeof(value)) != 0) {
            return -EFAULT;
        }
        kvm_tsc_offset_set(v, (int64_t)value);
    }
    return 0;
}

/** The ioctls of `v` but KVM_RUN, under its mutex and the big lock. */
static int32_t vcpu_state_ioctl(struct kvm_vcpu *v, uint32_t request, uintptr_t arg)
{
    struct kvm_mp_state mp;

    switch (request) {
    case KVM_GET_REGS:
        vcpu_save(v, KVM_PART_REGS);
        return copy_to_user((void __user *)arg, &v->regs, sizeof(v->regs)) ? -EFAULT : 0;
    case KVM_SET_REGS:
        return set_regs(v, arg);
    case KVM_GET_SREGS:
        return get_sregs(v, arg);
    case KVM_SET_SREGS:
        return set_sregs(v, arg);
    case KVM_GET_FPU:
        return get_fpu(v, arg);
    case KVM_SET_FPU:
        return set_fpu(v, arg);
    case KVM_GET_XSAVE:
    case KVM_GET_XSAVE2:
        return get_xsave(v, arg);
    case KVM_SET_XSAVE:
        return set_xsave(v, arg);
    case KVM_GET_XCRS:
        return get_xcrs(v, arg);
    case KVM_SET_XCRS:
        return set_xcrs(v, arg);
    case KVM_GET_MSRS:
        return msrs_io(v, arg, 0);
    case KVM_SET_MSRS:
        return msrs_io(v, arg, 1);
    case KVM_SET_CPUID2:
        return set_cpuid(v, arg);
    case KVM_GET_CPUID2:
        return get_cpuid(v, arg);
    case KVM_GET_MP_STATE:
        vcpu_save(v, KVM_PART_MP);
        if (v->vm->irqchip != KVM_IRQCHIP_NONE) {
            return kvm_lapic_ioctl(v, request, arg);
        }
        mp.mp_state = v->mp_state;
        return copy_to_user((void __user *)arg, &mp, sizeof(mp)) ? -EFAULT : 0;
    case KVM_SET_MP_STATE:
        if (v->vm->irqchip != KVM_IRQCHIP_NONE) {
            return kvm_lapic_ioctl(v, request, arg);
        }
        return set_mp_state(v, arg);
    case KVM_GET_VCPU_EVENTS:
        return get_events(v, arg);
    case KVM_SET_VCPU_EVENTS:
        return set_events(v, arg);
    case KVM_GET_DEBUGREGS:
        return get_debugregs(v, arg);
    case KVM_SET_DEBUGREGS:
        return set_debugregs(v, arg);
    case KVM_SET_GUEST_DEBUG:
        return set_guest_debug(v, arg);
    case KVM_INTERRUPT:
        return interrupt(v, arg);
    case KVM_NMI:
        if (v->nmi_queued < 2u) {
            v->nmi_queued++;
        }
        v->dirty |= KVM_PART_EVENTS;
        req_set(v, KVM_REQ_EVENT);
        return 0;
    case KVM_X86_SETUP_MCE:
        return setup_mce(v, arg);
    case KVM_X86_SET_MCE:
        return set_mce(v, arg);
    case KVM_SET_SIGNAL_MASK:
        return set_signal_mask(v, arg);
    case KVM_SET_VAPIC_ADDR:
        return set_vapic(v, arg);
    case KVM_GET_LAPIC:
    case KVM_SET_LAPIC:
        if (v->vm->irqchip == KVM_IRQCHIP_NONE) {
            return -EINVAL;
        }
        return kvm_lapic_ioctl(v, request, arg);
    case KVM_ENABLE_CAP:
        return vcpu_enable_cap(v, arg);
    case KVM_GET_SUPPORTED_HV_CPUID:
        return kvm_hv_get_cpuid(v, arg);
    case KVM_XEN_VCPU_GET_ATTR:
    case KVM_XEN_VCPU_SET_ATTR:
        return kvm_xen_vcpu_ioctl(v, request, arg);
    case KVM_SET_TSC_KHZ:
        return set_tsc_khz(v, arg);
    case KVM_GET_TSC_KHZ:
        return (int32_t)v->tsc_khz;
    case KVM_KVMCLOCK_CTRL:
        return kvm_clock_ctrl(v);
    case KVM_GET_STATS_FD:
        return kvm_stats_fd(v->vm, v);
    case KVM_PRE_FAULT_MEMORY:
        return pre_fault(v, arg);
    default:
        return device_attr(v, request, arg);
    }
}

/**
 * A vCPU's ioctl, called without the big lock as an unlocked file's
 * operation, or with a hold the system call's entry took for its own
 * reasons: a held descriptor or USB work (`syscall_entry_locked`).
 * KVM_RUN lets such a hold go for the whole run and takes it back at the
 * same depth before it returns, so that neither the guest nor the exits
 * the kernel handles keep the big lock, as Linux's KVM_RUN holds only the
 * vCPU's mutex (virt/kvm/kvm_main.c:4489-4515). Every other request runs
 * under the big lock. -EINTR when SIGKILL ends the wait for the vCPU's
 * mutex, with the entry's hold back at its depth.
 */
int32_t kvm_vcpu_ioctl(struct fd_entry *e, uint32_t request, uintptr_t arg)
{
    struct kvm_vcpu *v = (struct kvm_vcpu *)e->priv;
    uint32_t flags;
    int32_t r;
    int depth;

    if (!kvm_vm_caller_ok(v->vm)) {
        return -EIO;
    }
    if (((request >> 8) & 0xFFu) != KVMIO) {
        return -EINVAL;
    }
    if (request == KVM_RUN) {
        flags = kirq_save();
        depth = bkl_drop();
        kirq_restore(flags);
        r = vcpu_lock(v);
        if (r == 0) {
            r = vcpu_run(v, arg);
            vcpu_unlock(v);
        }
        flags = kirq_save();
        bkl_take(depth);
        kirq_restore(flags);
        return r;
    }
    if (vcpu_lock(v) != 0) {
        return -EINTR;
    }
    bkl_enter();
    r = vcpu_state_ioctl(v, request, arg);
    bkl_exit();
    vcpu_unlock(v);
    return r;
}
