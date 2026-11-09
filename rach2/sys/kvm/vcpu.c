/*
 * vcpu.c: a VM's vCPUs: the vCPU descriptor, its run pages mapped into the
 * caller, KVM_RUN's loop, and the ioctls of the vCPU's state.
 */

#include "kvm.h"

#include "../arch/arch.h"
#include "../fs/fd.h"
#include "../fs/file.h"
#include "../lib/kerrno.h"
#include "../lib/kirq.h"
#include "../lib/kstring.h"
#include "../mm/kheap.h"
#include "../mm/usercopy.h"
#include "../mm/vm.h"
#include "../proc/process.h"
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

/** Whether no ioctl of `arg`, a vCPU, is under way. */
static int vcpu_idle(void *arg)
{
    return !((struct kvm_vcpu *)arg)->busy;
}

/** Take `v` for one ioctl, waiting while another holds it, as Linux takes
 * the vCPU's mutex: 0, or -EINTR for a signal. */
static int32_t vcpu_lock(struct kvm_vcpu *v)
{
    while (v->busy) {
        if (wq_wait_event(&v->wq, vcpu_idle, v) < 0) {
            return -EINTR;
        }
    }
    v->busy = 1;
    return 0;
}

/** Let go of `v`. */
static void vcpu_unlock(struct kvm_vcpu *v)
{
    v->busy = 0;
    wq_wake(&v->wq, WQ_DATA);
}

/** Write back the parts `parts` of `v`'s state from its backend. */
static void vcpu_save(struct kvm_vcpu *v, uint32_t parts)
{
    if (kvm_backend && kvm_backend->save && v->has_run) {
        kvm_backend->save(v, parts);
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
 * Give `v` the state of a processor at power-on, as Linux's
 * kvm_vcpu_reset gives it: real mode at 0xFFFF0 below the reset vector's
 * segment, family 6 in EDX, CR0 with CD, NW and ET, the
 * local APIC at its default address, the boot vCPU's marked, the x87 and
 * SSE units at their initial state with XCR0 naming x87 alone, the debug
 * registers at reset, and the TSC at 0.
 */
static void vcpu_reset(struct kvm_vcpu *v)
{
    uint64_t bv = XFEATURE_X87 | XFEATURE_SSE;

    k_memset(&v->regs, 0, sizeof(v->regs));
    k_memset(&v->sregs, 0, sizeof(v->sregs));
    v->regs.rdx = 0x600; /* the signature Linux gives before KVM_SET_CPUID2 */
    v->regs.rip = 0xFFF0;
    v->regs.rflags = 0x2;
    reset_seg(&v->sregs.cs, 0xF000, 0xFFFF0000ull, 0xB);
    reset_seg(&v->sregs.ds, 0, 0, 0x3);
    reset_seg(&v->sregs.es, 0, 0, 0x3);
    reset_seg(&v->sregs.fs, 0, 0, 0x3);
    reset_seg(&v->sregs.gs, 0, 0, 0x3);
    reset_seg(&v->sregs.ss, 0, 0, 0x3);
    reset_seg(&v->sregs.tr, 0, 0, 0xB);
    v->sregs.tr.s = 0;
    reset_seg(&v->sregs.ldt, 0, 0, 0x2);
    v->sregs.ldt.s = 0;
    v->sregs.gdt.limit = 0xFFFF;
    v->sregs.idt.limit = 0xFFFF;
    v->sregs.cr0 = CR0_CD | CR0_NW | CR0_ET;
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
    k_memset(&v->events, 0, sizeof(v->events));
    v->nmi_queued = 0;
    k_memset(&v->debugregs, 0, sizeof(v->debugregs));
    v->debugregs.dr6 = DR6_RESET;
    v->debugregs.dr7 = DR7_RESET;
    k_memset(&v->guest_debug, 0, sizeof(v->guest_debug));
    v->mp_state = KVM_MP_STATE_RUNNABLE;
    v->tsc_offset = -(int64_t)arch_cycles();
    kvm_msr_reset(v);
    v->dirty = KVM_PART_ALL;
}

/** The page `pgoff` of the run pages of the vCPU `obj`, for a fault of a
 * mapping of them: 0, or -1 past them, which raises SIGBUS. */
static int run_page(void *obj, uint32_t pgoff, uintptr_t *phys)
{
    struct kvm_vcpu *v = (struct kvm_vcpu *)obj;

    if (pgoff >= KVM_RUN_PAGES) {
        return -1;
    }
    *phys = v->run_phys + (uintptr_t)pgoff * 4096u;
    return 0;
}

/** The last mapping of the vCPU `obj`'s run pages is gone: the reference
 * it held to the VM goes. */
static void run_put(void *obj)
{
    kvm_vm_put(((struct kvm_vcpu *)obj)->vm);
}

static const struct vm_device_ops run_vm_ops = {run_page, run_put};

/**
 * mmap of a vCPU: its run pages, shared and cached, which a fork's child
 * shares as Linux's does. The mapping holds a reference to the VM, so the
 * pages outlive the descriptor. 0, or vm_device_map's error, -EINVAL for a
 * private mapping among them.
 */
static int32_t vcpu_mmap(fd_entry_t *e, struct vma *vma)
{
    struct kvm_vcpu *v = (struct kvm_vcpu *)e->priv;
    int32_t r;

    kvm_vm_get(v->vm);
    r = vm_device_map(vma, &run_vm_ops, v, 0);
    if (r != 0) {
        kvm_vm_put(v->vm);
    }
    return r;
}

/** The last close of a vCPU's descriptor: its reference to the VM goes,
 * and the vCPU lives on with the VM, as on Linux. */
static void vcpu_close(fd_entry_t *e)
{
    kvm_vm_put(((struct kvm_vcpu *)e->priv)->vm);
    e->priv = NULL;
}

static const struct file_ops vcpu_ops = {NULL,       NULL,      NULL, kvm_vcpu_ioctl,
                                         vcpu_close, vcpu_mmap, NULL};

/** Write the decimal `n` at `p`, with its terminating NUL. */
static void put_decimal(char *p, uint32_t n)
{
    char digits[10];
    int i = 0;

    do {
        digits[i++] = (char)('0' + n % 10u);
        n /= 10u;
    } while (n != 0);
    while (i > 0) {
        *p++ = digits[--i];
    }
    *p = '\0';
}

void kvm_vcpu_free(struct kvm_vcpu *v)
{
    if (!v) {
        return;
    }
    if (kvm_backend && kvm_backend->vcpu_free) {
        kvm_backend->vcpu_free(v);
    }
    kvm_lapic_free(v);
    if (v->run) {
        kfree_pages(v->run, KVM_RUN_PAGES);
    }
    kfree(v->xsave_mem);
    kfree(v->cpuid);
    kfree(v);
}

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
    for (i = 0; i < vm->nr_vcpus; i++) {
        if (vm->vcpus[i]->id == id) {
            return -EEXIST;
        }
    }
    v = kzalloc(sizeof(*v));
    if (!v) {
        return -ENOMEM;
    }
    v->vm = vm;
    v->id = id;
    v->index = vm->nr_vcpus;
    v->cpu = -1;
    k_memcpy(v->name, "kvm-vcpu:", 9);
    put_decimal(v->name + 9, id);
    v->anon.name = v->name;
    wait_queue_init(&v->wq);
    r = -ENOMEM;
    v->run = kalloc_pages(KVM_RUN_PAGES, &v->run_phys);
    v->xsave_mem = kmalloc(KVM_XSAVE_SIZE + 64u);
    if (!v->run || !v->xsave_mem) {
        goto fail;
    }
    k_memset(v->run, 0, KVM_RUN_PAGES * 4096u);
    v->pio = (uint8_t *)v->run + KVM_PIO_PAGE_OFFSET * 4096u;
    v->xsave = (uint8_t *)(((uintptr_t)v->xsave_mem + 63u) & ~(uintptr_t)63u);
    vcpu_reset(v);
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
    vm->vcpus[vm->nr_vcpus++] = v;
    kvm_vm_get(vm);
    e = &current_proc->fds->entries[fd];
    e->type = FD_TYPE_ANON;
    e->ops = &vcpu_ops;
    e->priv = v;
    e->flags = FD_FLAG_READ | FD_FLAG_WRITE | FD_FLAG_CLOEXEC;
    return fd;
fail:
    kvm_vcpu_free(v);
    return r;
}

void kvm_vcpu_kick(struct kvm_vcpu *v, uint32_t request)
{
    v->requests |= request;
    if (v->mode == KVM_MODE_IN_GUEST && v->cpu >= 0 && (unsigned int)v->cpu != arch_cpu_id()) {
        smp_kick(v->cpu);
    }
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
    v->requests |= KVM_REQ_EVENT;
}

/** Fill the run page's fields every return of KVM_RUN gives, as Linux's
 * post_kvm_run_save. */
static void run_post(struct kvm_vcpu *v)
{
    struct kvm_run *run = v->run;

    run->if_flag = (uint8_t)(v->if_flag != 0);
    run->cr8 = v->sregs.cr8;
    run->apic_base = v->sregs.apic_base;
    run->ready_for_interrupt_injection = (uint8_t)(v->vm->irqchip == KVM_IRQCHIP_NONE &&
                                                   v->irq_allowed && !v->events.interrupt.injected);
    run->flags = 0;
}

/** Give the backend the caller's answer to the read of the last exit: the
 * IN's bytes from the port I/O page, or the MMIO read's from the run page. */
static void run_complete(struct kvm_vcpu *v)
{
    struct kvm_exit *x = &v->exit;
    uint32_t n;

    if (x->kind == KVM_X_IO) {
        n = x->io.count * x->io.size;
        k_memcpy(x->data, v->pio, n < sizeof(x->data) ? n : sizeof(x->data));
    } else if (x->kind == KVM_X_MMIO) {
        k_memcpy(x->mmio.data, v->run->u.mmio.data, sizeof(x->mmio.data));
    }
    v->pending = 0;
    if (kvm_backend->complete) {
        kvm_backend->complete(v);
    }
}

/**
 * Give the caller the exit in `v->exit`: 1 when it goes to the caller,
 * with the run page filled, 0 when the kernel took it and the guest enters
 * again, or -errno.
 */
static int32_t run_exit(struct kvm_vcpu *v)
{
    struct kvm_run *run = v->run;
    struct kvm_exit *x = &v->exit;
    uint32_t n;
    int r;

    switch (x->kind) {
    case KVM_X_AGAIN:
        return 0;
    case KVM_X_IO:
        if (x->io.direction == KVM_EXIT_IO_OUT && kvm_iobus_write(v)) {
            return 0;
        }
        run->exit_reason = KVM_EXIT_IO;
        run->u.io = x->io;
        run->u.io.data_offset = KVM_PIO_PAGE_OFFSET * 4096u;
        n = x->io.count * x->io.size;
        if (n > 4096u) {
            n = 4096u;
        }
        if (x->io.direction == KVM_EXIT_IO_OUT) {
            k_memcpy(v->pio, x->data, n);
        } else {
            v->pending = 1;
        }
        return 1;
    case KVM_X_MMIO:
        if (x->mmio.is_write && kvm_iobus_write(v)) {
            return 0;
        }
        run->exit_reason = KVM_EXIT_MMIO;
        run->u.mmio = x->mmio;
        v->pending = !x->mmio.is_write;
        return 1;
    case KVM_X_HLT:
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
        run->exit_reason = KVM_EXIT_IRQ_WINDOW_OPEN;
        return 1;
    case KVM_X_INTERNAL:
        run->exit_reason = KVM_EXIT_INTERNAL_ERROR;
        run->u.internal = x->internal;
        if (run->u.internal.ndata > 16u) {
            run->u.internal.ndata = 16u;
        }
        return 1;
    case KVM_X_FAIL_ENTRY:
        run->exit_reason = KVM_EXIT_FAIL_ENTRY;
        run->u.fail_entry.hardware_entry_failure_reason = x->hw_reason;
        run->u.fail_entry.cpu = (uint32_t)v->cpu;
        return 1;
    case KVM_X_DEBUG:
        run->exit_reason = KVM_EXIT_DEBUG;
        run->u.debug = x->debug;
        return 1;
    case KVM_X_SYSTEM_EVENT:
        run->exit_reason = KVM_EXIT_SYSTEM_EVENT;
        run->u.system_event = x->system_event;
        return 1;
    default:
        break;
    }
    run->exit_reason = KVM_EXIT_INTERNAL_ERROR;
    k_memset(&run->u.internal, 0, sizeof(run->u.internal));
    run->u.internal.suberror = KVM_INTERNAL_ERROR_UNEXPECTED_EXIT_REASON;
    run->u.internal.ndata = 1;
    run->u.internal.data[0] = (uint64_t)x->kind;
    return 1;
}

/**
 * Enter the guest once, with interrupts off and without the big lock, as
 * the backend's `enter` needs, and take both back after: 0, -EAGAIN when a
 * request came between the check and the entry, or the backend's error.
 * The vCPU is IN_GUEST from before its requests are read, so a kick that
 * comes after the read finds it there and interrupts the processor.
 */
static int32_t run_enter(struct kvm_vcpu *v)
{
    uint32_t flags = kirq_save();
    int depth;
    int32_t r;

    v->cpu = (int32_t)arch_cpu_id();
    v->mode = KVM_MODE_IN_GUEST;
    if (v->requests & (KVM_REQ_EXIT | KVM_REQ_TRIPLE_FAULT)) {
        v->mode = KVM_MODE_OUTSIDE;
        kirq_restore(flags);
        return -EAGAIN;
    }
    v->has_run = 1;
    depth = bkl_drop();
    r = kvm_backend->enter(v);
    v->mode = KVM_MODE_OUTSIDE;
    bkl_take(depth);
    kirq_restore(flags);
    return r;
}

/**
 * KVM_RUN's loop, as Linux's kvm_arch_vcpu_ioctl_run and vcpu_run: give
 * the backend the answer to the last exit's read, take CR8 from the run
 * page with no local APIC in the kernel, end at once under immediate_exit,
 * then enter until an exit the caller answers, a signal, or a request to
 * leave. Returns 0 with `exit_reason` set, -EINTR with it KVM_EXIT_INTR
 * for a signal or a kick, or -errno.
 */
static int32_t run_loop(struct kvm_vcpu *v)
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
    if (run->immediate_exit) {
        return -EINTR;
    }
    for (;;) {
        if (v->requests & KVM_REQ_TRIPLE_FAULT) {
            v->requests &= ~KVM_REQ_TRIPLE_FAULT;
            run->exit_reason = KVM_EXIT_SHUTDOWN;
            return 0;
        }
        if ((v->requests & KVM_REQ_EXIT) || signal_pending()) {
            v->requests &= ~KVM_REQ_EXIT;
            run->exit_reason = KVM_EXIT_INTR;
            return -EINTR;
        }
        if (v->requests & KVM_REQ_EVENT) {
            v->requests &= ~KVM_REQ_EVENT;
            if (kvm_backend->inject) {
                kvm_backend->inject(v);
            }
        }
        r = run_enter(v);
        if (r == -EAGAIN) {
            continue;
        }
        if (r != 0) {
            return r;
        }
        r = kvm_backend->exit ? kvm_backend->exit(v) : -EOPNOTSUPP;
        if (r != 0) {
            return r;
        }
        r = run_exit(v);
        if (r != 0) {
            return r > 0 ? 0 : r;
        }
    }
}

/** KVM_RUN, whose argument is 0, under the signal mask of
 * KVM_SET_SIGNAL_MASK when one is set, with the old mask back after, as
 * Linux's kvm_sigset_activate and kvm_sigset_deactivate swap them. */
static int32_t vcpu_run(struct kvm_vcpu *v, uintptr_t arg)
{
    uint64_t old = 0;
    int32_t r;

    if (arg) {
        return -EINVAL;
    }
    if (v->has_sigmask) {
        old = current_proc->sig_blocked;
        signal_set_blocked(v->sigmask);
    }
    r = run_loop(v);
    if (v->has_sigmask) {
        signal_set_blocked(old);
    }
    run_post(v);
    return r;
}

/** Whether `v`'s CPUID offers the feature in bit `bit` of the register
 * `reg` (0 EAX to 3 EDX) of `function`'s subleaf 0. */
static int guest_has(const struct kvm_vcpu *v, uint32_t function, int reg, int bit)
{
    const struct kvm_cpuid_entry2 *e = kvm_cpuid_find(v, function, 0);
    uint32_t r;

    if (!e) {
        return 0;
    }
    r = reg == 0 ? e->eax : reg == 1 ? e->ebx : reg == 2 ? e->ecx : e->edx;
    return (r >> bit) & 1u;
}

/** The guest's physical address bits, from leaf 0x80000008 of its CPUID,
 * 36 without it, as Linux's cpuid_query_maxphyaddr. */
static uint32_t guest_maxphyaddr(const struct kvm_vcpu *v)
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

/** The CR4 bits `v`'s CPUID reserves, as Linux's __cr4_reserved_bits:
 * VMXE and SMXE always, since no nested guest is offered. */
static uint64_t cr4_reserved(const struct kvm_vcpu *v)
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

/** Whether `s` is a state KVM_SET_SREGS takes, as Linux's
 * kvm_is_valid_sregs, kvm_is_valid_cr0 and kvm_is_valid_cr4 judge it,
 * with the APIC base's reserved bits as kvm_set_apic_base judges them. */
static int sregs_valid(const struct kvm_vcpu *v, const struct kvm_sregs *s)
{
    uint64_t gpa_reserved = ~0ull << guest_maxphyaddr(v);
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
    if (s->cr4 & cr4_reserved(v)) {
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

/** KVM_GET_SREGS: the special registers, with the interrupt waiting for
 * injection, if not a software one, set in the bitmap. */
static int32_t get_sregs(struct kvm_vcpu *v, uintptr_t arg)
{
    struct kvm_sregs s;

    vcpu_save(v, KVM_PART_SREGS | KVM_PART_EVENTS);
    s = v->sregs;
    k_memset(s.interrupt_bitmap, 0, sizeof(s.interrupt_bitmap));
    if (v->events.interrupt.injected && !v->events.interrupt.soft) {
        s.interrupt_bitmap[v->events.interrupt.nr / 64u] |= 1ull << (v->events.interrupt.nr % 64u);
    }
    return copy_to_user((void __user *)arg, &s, sizeof(s)) ? -EFAULT : 0;
}

/** KVM_SET_SREGS: the special registers, and the first interrupt of the
 * bitmap queued for injection. 0, -EINVAL for a state Linux refuses, or
 * -EFAULT. */
static int32_t set_sregs(struct kvm_vcpu *v, uintptr_t arg)
{
    struct kvm_sregs s;
    uint32_t i;

    if (copy_from_user(&s, (const void __user *)arg, sizeof(s)) != 0) {
        return -EFAULT;
    }
    if (!sregs_valid(v, &s)) {
        return -EINVAL;
    }
    v->sregs = s;
    k_memset(v->sregs.interrupt_bitmap, 0, sizeof(v->sregs.interrupt_bitmap));
    v->dirty |= KVM_PART_SREGS;
    for (i = 0; i < KVM_NR_INTERRUPTS; i++) {
        if ((s.interrupt_bitmap[i / 64u] >> (i % 64u)) & 1u) {
            v->events.interrupt.injected = 1;
            v->events.interrupt.nr = (uint8_t)i;
            v->events.interrupt.soft = 0;
            v->dirty |= KVM_PART_EVENTS;
            v->requests |= KVM_REQ_EVENT;
            break;
        }
    }
    return 0;
}

/** KVM_GET_FPU: the legacy region of the XSAVE image, without MXCSR, which
 * Linux's kvm_arch_vcpu_ioctl_get_fpu leaves out too. */
static int32_t get_fpu(struct kvm_vcpu *v, uintptr_t arg)
{
    struct kvm_fpu f;
    const uint8_t *x = v->xsave;

    vcpu_save(v, KVM_PART_FPU);
    k_memset(&f, 0, sizeof(f));
    k_memcpy(f.fpr, x + 32, sizeof(f.fpr));
    k_memcpy(&f.fcw, x + 0, 2);
    k_memcpy(&f.fsw, x + 2, 2);
    f.ftwx = x[4];
    k_memcpy(&f.last_opcode, x + 6, 2);
    k_memcpy(&f.last_ip, x + 8, 8);
    k_memcpy(&f.last_dp, x + 16, 8);
    k_memcpy(f.xmm, x + 160, sizeof(f.xmm));
    return copy_to_user((void __user *)arg, &f, sizeof(f)) ? -EFAULT : 0;
}

/** KVM_SET_FPU: the same fields into the legacy region, with x87 and SSE
 * then marked in use so the next entry loads them. */
static int32_t set_fpu(struct kvm_vcpu *v, uintptr_t arg)
{
    struct kvm_fpu f;
    uint8_t *x = v->xsave;
    uint64_t bv;

    if (copy_from_user(&f, (const void __user *)arg, sizeof(f)) != 0) {
        return -EFAULT;
    }
    vcpu_save(v, KVM_PART_FPU);
    k_memcpy(x + 32, f.fpr, sizeof(f.fpr));
    k_memcpy(x + 0, &f.fcw, 2);
    k_memcpy(x + 2, &f.fsw, 2);
    x[4] = f.ftwx;
    k_memcpy(x + 6, &f.last_opcode, 2);
    k_memcpy(x + 8, &f.last_ip, 8);
    k_memcpy(x + 16, &f.last_dp, 8);
    k_memcpy(x + 160, f.xmm, sizeof(f.xmm));
    k_memcpy(&bv, x + XSAVE_HDR, sizeof(bv));
    bv |= XFEATURE_X87 | XFEATURE_SSE;
    k_memcpy(x + XSAVE_HDR, &bv, sizeof(bv));
    v->dirty |= KVM_PART_FPU;
    return 0;
}

/** KVM_GET_XSAVE: the XSAVE image in its standard form, with only the
 * components the guest's CPUID allows marked in use. */
static int32_t get_xsave(struct kvm_vcpu *v, uintptr_t arg)
{
    uint64_t bv;
    int32_t r;
    uint8_t *x;

    if (kvm_xcr0_supported() == 0) {
        return -EINVAL;
    }
    vcpu_save(v, KVM_PART_FPU);
    x = kmalloc(KVM_XSAVE_SIZE);
    if (!x) {
        return -ENOMEM;
    }
    k_memcpy(x, v->xsave, KVM_XSAVE_SIZE);
    k_memcpy(&bv, x + XSAVE_HDR, sizeof(bv));
    bv &= kvm_xcr0_guest(v);
    k_memcpy(x + XSAVE_HDR, &bv, sizeof(bv));
    r = copy_to_user((void __user *)arg, x, KVM_XSAVE_SIZE) ? -EFAULT : 0;
    kfree(x);
    return r;
}

/**
 * KVM_SET_XSAVE: an XSAVE image in its standard form, which Linux takes
 * when its header marks only components the guest's CPUID allows, its
 * compaction word and reserved bytes are zero, and, with SSE or AVX marked,
 * its MXCSR sets no reserved bit. 0, -EINVAL, -ENOMEM or -EFAULT.
 */
static int32_t set_xsave(struct kvm_vcpu *v, uintptr_t arg)
{
    uint64_t bv, comp;
    uint32_t mxcsr, i;
    int32_t r = -EINVAL;
    uint8_t *x;

    if (kvm_xcr0_supported() == 0) {
        return -EINVAL;
    }
    x = kmalloc(KVM_XSAVE_SIZE);
    if (!x) {
        return -ENOMEM;
    }
    if (copy_from_user(x, (const void __user *)arg, KVM_XSAVE_SIZE) != 0) {
        r = -EFAULT;
        goto out;
    }
    k_memcpy(&bv, x + XSAVE_HDR, sizeof(bv));
    k_memcpy(&comp, x + XSAVE_HDR + 8, sizeof(comp));
    k_memcpy(&mxcsr, x + 24, sizeof(mxcsr));
    if ((bv & ~kvm_xcr0_guest(v)) || comp != 0) {
        goto out;
    }
    for (i = XSAVE_HDR + 16; i < XSAVE_HDR + 64; i++) {
        if (x[i] != 0) {
            goto out;
        }
    }
    if ((bv & (XFEATURE_SSE | XFEATURE_YMM)) && (mxcsr & ~MXCSR_MASK)) {
        goto out;
    }
    k_memcpy(v->xsave, x, KVM_XSAVE_SIZE);
    v->dirty |= KVM_PART_FPU;
    r = 0;
out:
    kfree(x);
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
    struct kvm_xcrs x;

    vcpu_save(v, KVM_PART_FPU);
    k_memset(&x, 0, sizeof(x));
    if (kvm_xcr0_supported() != 0) {
        x.nr_xcrs = 1;
        x.xcrs[0].xcr = 0;
        x.xcrs[0].value = v->xcr0;
    }
    return copy_to_user((void __user *)arg, &x, sizeof(x)) ? -EFAULT : 0;
}

/** KVM_SET_XCRS: the first entry for XCR0 sets it, and Linux ignores the
 * rest. 0, -EINVAL without XSAVE, for flags, too many entries or a value
 * XSETBV refuses, or -EFAULT. */
static int32_t set_xcrs(struct kvm_vcpu *v, uintptr_t arg)
{
    struct kvm_xcrs x;
    uint32_t i;

    if (copy_from_user(&x, (const void __user *)arg, sizeof(x)) != 0) {
        return -EFAULT;
    }
    if (kvm_xcr0_supported() == 0 || x.nr_xcrs > KVM_MAX_XCRS || x.flags) {
        return -EINVAL;
    }
    for (i = 0; i < x.nr_xcrs; i++) {
        if (x.xcrs[i].xcr == 0) {
            if (!xcr0_valid(v, x.xcrs[i].value)) {
                return -EINVAL;
            }
            v->xcr0 = x.xcrs[i].value;
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
    if (!set) {
        vcpu_save(v, KVM_PART_MSRS | KVM_PART_TSC);
    }
    for (i = 0; i < head.nmsrs; i++) {
        if (set ? kvm_msr_set(v, ent[i].index, ent[i].data) != 0
                : kvm_msr_get(v, ent[i].index, &ent[i].data) != 0) {
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
 * takes only unchanged. 0, -E2BIG past KVM_MAX_CPUID_ENTRIES, -EINVAL for a
 * change after a run, -ENOMEM or -EFAULT.
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
    if (v->has_run) {
        int same = head.nent == v->cpuid_nent && bytes_equal(ent, v->cpuid, bytes);
        kfree(ent);
        return same ? 0 : -EINVAL;
    }
    kfree(v->cpuid);
    v->cpuid = ent;
    v->cpuid_nent = head.nent;
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

/** KVM_GET_VCPU_EVENTS: the events waiting, with an exception reported
 * as injected, since the caller has not enabled exception payloads. */
static int32_t get_events(struct kvm_vcpu *v, uintptr_t arg)
{
    struct kvm_vcpu_events ev;

    vcpu_save(v, KVM_PART_EVENTS);
    ev = v->events;
    ev.exception.injected = (uint8_t)(ev.exception.injected || ev.exception.pending);
    ev.exception.pending = 0;
    ev.exception_has_payload = 0;
    ev.exception_payload = 0;
    ev.nmi.pending = (uint8_t)(v->nmi_queued != 0);
    ev.flags =
        KVM_VCPUEVENT_VALID_NMI_PENDING | KVM_VCPUEVENT_VALID_SHADOW | KVM_VCPUEVENT_VALID_SMM;
    k_memset(&ev.smi, 0, sizeof(ev.smi));
    k_memset(&ev.triple_fault, 0, sizeof(ev.triple_fault));
    k_memset(ev.reserved, 0, sizeof(ev.reserved));
    return copy_to_user((void __user *)arg, &ev, sizeof(ev)) ? -EFAULT : 0;
}

/**
 * KVM_SET_VCPU_EVENTS, as Linux's kvm_vcpu_ioctl_x86_set_vcpu_events
 * takes them with neither exception payloads nor triple-fault events
 * enabled and no SMM: the exception, the interrupt and the NMI's state
 * always, the NMI's pending count, the SIPI vector and the interrupt
 * shadow when their flags say so. -EINVAL for another flag, a payload, an
 * exception past 31 or the NMI's vector, or SMM; -EFAULT.
 */
static int32_t set_events(struct kvm_vcpu *v, uintptr_t arg)
{
    struct kvm_vcpu_events ev;

    if (copy_from_user(&ev, (const void __user *)arg, sizeof(ev)) != 0) {
        return -EFAULT;
    }
    if (ev.flags & ~(KVM_VCPUEVENT_VALID_NMI_PENDING | KVM_VCPUEVENT_VALID_SIPI_VECTOR |
                     KVM_VCPUEVENT_VALID_SHADOW | KVM_VCPUEVENT_VALID_SMM)) {
        return -EINVAL;
    }
    ev.exception.pending = 0;
    if (ev.exception.injected && (ev.exception.nr > 31 || ev.exception.nr == NMI_VECTOR)) {
        return -EINVAL;
    }
    if ((ev.flags & KVM_VCPUEVENT_VALID_SMM) &&
        (ev.smi.smm || ev.smi.pending || ev.smi.smm_inside_nmi)) {
        return -EINVAL;
    }
    vcpu_save(v, KVM_PART_EVENTS);
    v->events.exception = ev.exception;
    v->events.interrupt.injected = ev.interrupt.injected;
    v->events.interrupt.nr = ev.interrupt.nr;
    v->events.interrupt.soft = ev.interrupt.soft;
    if (ev.flags & KVM_VCPUEVENT_VALID_SHADOW) {
        v->events.interrupt.shadow = ev.interrupt.shadow;
    }
    v->events.nmi.injected = ev.nmi.injected;
    v->events.nmi.masked = ev.nmi.masked;
    if (ev.flags & KVM_VCPUEVENT_VALID_NMI_PENDING) {
        v->nmi_queued = ev.nmi.pending ? 1u : 0u;
    }
    if (ev.flags & KVM_VCPUEVENT_VALID_SIPI_VECTOR) {
        v->events.sipi_vector = ev.sipi_vector;
    }
    v->dirty |= KVM_PART_EVENTS;
    v->requests |= KVM_REQ_EVENT;
    return 0;
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

/** KVM_SET_GUEST_DEBUG, which the backend acts on at the next entry: an
 * INJECT flag queues #DB or #BP, -EBUSY while an exception waits; -EINVAL
 * for a flag Linux does not know. */
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
    v->requests |= KVM_REQ_EVENT;
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
            v->requests |= KVM_REQ_TRIPLE_FAULT;
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

/** KVM_ENABLE_CAP of a vCPU: Linux's x86 vCPU capabilities are Hyper-V's
 * and the enforcement of paravirtual features, which are not offered:
 * -EINVAL, as for one not enabled. */
static int32_t vcpu_enable_cap(uintptr_t arg)
{
    struct kvm_enable_cap cap;

    if (copy_from_user(&cap, (const void __user *)arg, sizeof(cap)) != 0) {
        return -EFAULT;
    }
    return -EINVAL;
}

/** The ioctls of `v` but KVM_RUN, under its lock. */
static int32_t vcpu_state_ioctl(struct kvm_vcpu *v, uint32_t request, uintptr_t arg)
{
    struct kvm_mp_state mp;

    switch (request) {
    case KVM_GET_REGS:
        vcpu_save(v, KVM_PART_REGS);
        return copy_to_user((void __user *)arg, &v->regs, sizeof(v->regs)) ? -EFAULT : 0;
    case KVM_SET_REGS:
        if (copy_from_user(&v->regs, (const void __user *)arg, sizeof(v->regs)) != 0) {
            return -EFAULT;
        }
        v->dirty |= KVM_PART_REGS;
        v->requests |= KVM_REQ_EVENT;
        return 0;
    case KVM_GET_SREGS:
        return get_sregs(v, arg);
    case KVM_SET_SREGS:
        return set_sregs(v, arg);
    case KVM_GET_FPU:
        return get_fpu(v, arg);
    case KVM_SET_FPU:
        return set_fpu(v, arg);
    case KVM_GET_XSAVE:
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
        mp.mp_state = v->mp_state;
        return copy_to_user((void __user *)arg, &mp, sizeof(mp)) ? -EFAULT : 0;
    case KVM_SET_MP_STATE:
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
        v->requests |= KVM_REQ_EVENT;
        return 0;
    case KVM_X86_SETUP_MCE:
        return setup_mce(v, arg);
    case KVM_X86_SET_MCE:
        return set_mce(v, arg);
    case KVM_SET_SIGNAL_MASK:
        return set_signal_mask(v, arg);
    case KVM_GET_LAPIC:
    case KVM_SET_LAPIC:
        if (v->vm->irqchip == KVM_IRQCHIP_NONE) {
            return -EINVAL;
        }
        return kvm_lapic_ioctl(v, request, arg);
    case KVM_ENABLE_CAP:
        return vcpu_enable_cap(arg);
    default:
        return -EINVAL;
    }
}

int32_t kvm_vcpu_ioctl(struct fd_entry *e, uint32_t request, uintptr_t arg)
{
    struct kvm_vcpu *v = (struct kvm_vcpu *)e->priv;
    int32_t r;

    if (!kvm_vm_caller_ok(v->vm)) {
        return -EIO;
    }
    if (((request >> 8) & 0xFFu) != KVMIO) {
        return -EINVAL;
    }
    r = vcpu_lock(v);
    if (r != 0) {
        return r;
    }
    r = request == KVM_RUN ? vcpu_run(v, arg) : vcpu_state_ioctl(v, request, arg);
    vcpu_unlock(v);
    return r;
}
