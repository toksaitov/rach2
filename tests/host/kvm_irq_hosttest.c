/**
 * Check KVM's interrupts in the kernel: the local APIC against the SDM's
 * register rules, the split controller's routes, MSIs, irqfds over the
 * kernel's eventfd, ioeventfds, the PIC, IOAPIC and PIT.
 * --irqchip-only runs the full-controller rows alone.
 *
 * host-deps: tests/host/compat/spinlock_host.c
 * host-cflags: -DRACH2_HOST_X86_64 -DRACH2_HOST_CPU
 *
 * vlapic.c, irq.c, iobus.c and io/eventfd.c are the kernel's, included
 * whole. The VM is a record of four vCPUs made here, with a guest CPUID
 * that offers x2APIC and the TSC deadline. The clocks are one model: the
 * host's ns, the tick of 1 ms and a TSC of 2 counts a ns, so a halted wait
 * that sleeps to its tick finds the timer expired.
 *
 * The rows: the reset state, the boot vCPU's LINT0 in ExtINT mode; MMIO
 * reads of whole registers and of bytes, a read across registers, the
 * write-only EOI and the reserved ARBPRI; TPR, ISR and PPR by priority
 * class; fixed MSIs into IRR, held by PPR, acknowledged into ISR and ended
 * by EOI; TMR for level-triggered ones; physical, flat, cluster and
 * broadcast destinations and lowest priority by Linux's vector hashing;
 * INIT and SIPI through the ICR, an INIT and its SIPI taken together, an
 * SIPI to a running vCPU dropped and SIPI_RECEIVED from KVM_SET_MP_STATE;
 * NMIs; the timer one-shot, periodic
 * with Linux's 200 us floor, TSC-deadline and TSC-deadline refused without
 * the CPUID bit; SPIV's software disable masking the LVT; x2APIC mode, its
 * MSRs, its #GP cases, its self IPI and IPI, its MMIO hole and its moves
 * between modes; KVM_GET_LAPIC and KVM_SET_LAPIC with the x2APIC id and
 * the timer's count; KVM_CAP_X2APIC_API's 32-bit ids, high MSI addresses
 * and broadcast quirk; KVM_INTERRUPT's ExtINT; the halted vCPU woken by an
 * interrupt, by its timer's tick, or ended by the signal-work flag alone;
 * the split controller's enable, the routes' refusals, a SINT route raising
 * Hyper-V's SynIC, KVM_IRQ_LINE_STATUS and the EOI exit of a
 * level-triggered I/O APIC route, marked by each vCPU's scan before entry
 * for the vCPU it targets and one with its vector in service, and for no
 * other; KVM_SIGNAL_MSI; irqfds raised by a write to the eventfd, following
 * a new route, refused twice or with RESAMPLE, deassigned, raised at once
 * by a counter already up, freed by the eventfd's last close, by the VM's
 * end and after a close that left the hangup to run;
 * ioeventfds on ports and MMIO, with and without a data match, any length,
 * string OUTs, their refusals and collisions, a full bus, and deassign;
 * coalesced zones of MMIO and of ports, queued on the VM's ring until one
 * slot is left, from any start, and removed by a range they hold; and every
 * allocation given back.
 */
#include "io/eventfd.c"
#include "kvm/iobus.c"
#include "kvm/irq.c"
#include "kvm/i8259.c"
#include "kvm/ioapic.c"
#include "kvm/i8254.c"
#include "kvm/vlapic.c"
#include "lib/kstring.h"

extern int printf(const char *format, ...);
extern void *malloc(unsigned long n);
extern void *aligned_alloc(unsigned long align, unsigned long n);
extern void free(void *p);
extern int strcmp(const char *a, const char *b);

static int failures;

/** Count and print a failure when `condition` is false. */
#define CHECK(condition, detail)                              \
    do {                                                      \
        if (!(condition)) {                                   \
            printf("kvm-irq-hosttest: failed: %s\n", detail); \
            failures++;                                       \
        }                                                     \
    } while (0)

static process_t caller;
struct percpu cpu_table[NR_CPUS] = {{.current = &caller}};
unsigned int host_cpu;

/* The heap's live allocations. */
static int live;
static int allocation_fail = -1; /* allocations before failure, -1 unlimited */

/** kmalloc over the host's allocator, counted. */
void *kmalloc(size_t size)
{
    void *p;

    if (allocation_fail == 0) {
        return NULL;
    }
    if (allocation_fail > 0) {
        allocation_fail--;
    }
    p = malloc(size ? size : 1);
    if (p) {
        live++;
    }
    return p;
}

/** kzalloc over the host's allocator, counted. */
void *kzalloc(size_t size)
{
    void *p = kmalloc(size);

    if (p) {
        k_memset(p, 0, size);
    }
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

/** kalloc_pages over the host's allocator, counted as kmalloc counts: the
 * APIC's register page. A host address is its own physical address. */
void *kalloc_pages(uint32_t count, uintptr_t *phys)
{
    void *p;

    if (allocation_fail == 0) {
        return NULL;
    }
    if (allocation_fail > 0) {
        allocation_fail--;
    }
    p = aligned_alloc(4096, 4096ul * count);
    if (p) {
        live++;
    }
    if (phys) {
        *phys = (uintptr_t)p;
    }
    return p;
}

/** kfree_pages, counted. */
void kfree_pages(void *p, uint32_t count)
{
    (void)count;
    kfree(p);
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

/* The interrupt flag of the model's processor. */
static uint32_t irq_on = 1;

uint32_t kirq_save(void)
{
    uint32_t f = irq_on;

    irq_on = 0;
    return f;
}

void kirq_restore(uint32_t flags)
{
    irq_on = flags;
}

unsigned int arch_atomic_cmpxchg(volatile unsigned int *p, unsigned int old, unsigned int value)
{
    unsigned int cur = *p;

    if (cur == old) {
        *p = value;
    }
    return cur;
}

unsigned int arch_atomic_xchg(volatile unsigned int *p, unsigned int value)
{
    unsigned int cur = *p;

    *p = value;
    return cur;
}

/* The clocks: the host's ns, its tick of 1 ms, and a TSC of 2 counts a ns
 * from `tsc_base`. */
static uint64_t mono_ns = 7000000000ull;
static const uint64_t tsc_base = 123456;

uint64_t kvm_host_ns(void)
{
    return mono_ns;
}

uint64_t arch_cycles(void)
{
    return tsc_base + mono_ns * 2u;
}

/* pvclock.c's guest TSC, which no vCPU here has scaled. */
uint64_t kvm_tsc_read(const struct kvm_vcpu *v)
{
    return arch_cycles() + (uint64_t)v->tsc_offset;
}

uint64_t tick_count(void)
{
    return mono_ns / TICK_NS;
}

uint32_t timer_calibrate(void)
{
    return 2u * TICK_NS;
}

/* Hyper-V's SynIC, which kvm_hyperv_hosttest checks: no vector is auto-EOI,
 * no timer is due, and a SINT raised through a route is recorded. */
static uint32_t sint_vpidx, sint_num, sint_raises;

int kvm_hv_synic_auto_eoi(struct kvm_vcpu *v, uint32_t vector)
{
    (void)v;
    (void)vector;
    return 0;
}

void kvm_hv_synic_eoi(struct kvm_vcpu *v, uint32_t vector)
{
    (void)v;
    (void)vector;
}

int kvm_hv_synic_set_irq(struct kvm_vm *vm, uint32_t vpidx, uint32_t sint)
{
    (void)vm;
    sint_vpidx = vpidx;
    sint_num = sint;
    sint_raises++;
    return 1;
}

void kvm_hv_stimers(struct kvm_vcpu *v)
{
    (void)v;
}

uint64_t kvm_hv_stimer_next(const struct kvm_vcpu *v)
{
    (void)v;
    return 0;
}

/* The host processor: the TSC deadline in leaf 1. */
void kcpu_id(uint32_t leaf, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d)
{
    *a = *b = *d = 0;
    *c = leaf == 1 ? (1u << 24) : 0;
}

/* The waits: an eventfd's ends with -EINTR when its condition is false;
 * a halted vCPU's is counted in `waits`, with `sleep_to_tick` sleeps to
 * its tick first, and is counted in `endless` when nothing would end it. */
static int waits, sleep_to_tick, wakes_done, endless;

/** An eventfd's wait: its condition, or a signal. */
int wq_wait_event(wait_queue_t *wq, wq_cond_fn cond, void *arg)
{
    (void)wq;
    return cond(arg) ? 0 : -EINTR;
}

/**
 * A halted vCPU's wait, as wait_queue.c's WQ_SIGFLAG: its condition ends it
 * with 0, and the caller's signal-work flag, read with no lock, with
 * -EINTR, as Linux's kvm_vcpu_check_block (virt/kvm/kvm_main.c:3729-3747).
 * A wait that neither ends would sleep for ever: it is counted in
 * `endless` and ends with -EINTR, so that a row sees it.
 */
int wq_wait_event_sigflag(wait_queue_t *wq, wq_cond_fn cond, void *arg, const char *name)
{
    (void)wq;
    (void)name;
    waits++;
    if (cond(arg)) {
        return 0;
    }
    if (sleep_to_tick && current_proc->wakeup_tick) {
        mono_ns = current_proc->wakeup_tick * TICK_NS;
        if (cond(arg)) {
            return 0;
        }
    }
    if (*(volatile uint32_t *)&current_proc->sig_woken) {
        return -EINTR;
    }
    endless++;
    return -EINTR;
}

void wq_wake(wait_queue_t *wq, uint32_t kinds)
{
    (void)wq;
    (void)kinds;
    wakes_done++;
}

void wait_queue_init(wait_queue_t *wq)
{
    wq->head = NULL;
}

/* The descriptors: 3 and 5 are eventfds, the rest not open. */
static fd_entry_t efd_entry, efd2_entry;
static struct open_file efd_desc;

fd_entry_t *fd_get(fd_table_t *table, int fd)
{
    (void)table;
    return fd == 3 ? &efd_entry : fd == 5 ? &efd2_entry : NULL;
}

int fd_alloc(fd_table_t *table)
{
    (void)table;
    return -1;
}

/** An eventfd on the descriptor `e`, nonblocking, at 0. */
static struct eventfd *efd_open(fd_entry_t *e)
{
    struct eventfd *ev = kzalloc(sizeof(*ev));

    ev->refs = 1;
    wait_queue_init(&ev->wq);
    efd_desc.status_flags = OFD_NONBLOCK;
    k_memset(e, 0, sizeof(*e));
    e->ofd = &efd_desc;
    e->priv = ev;
    e->ops = &eventfd_ops;
    e->flags = FD_FLAG_READ | FD_FLAG_WRITE;
    return ev;
}

/** A program's write of `value` to the eventfd on `e`. */
static int32_t efd_write(fd_entry_t *e, uint64_t value)
{
    return eventfd_write(e, (const char *)&value, sizeof(value));
}

/* The vCPUs' side: kicks, the INIT reset and the guest's CPUID. */
static int kicks, init_resets;

void kvm_vcpu_kick(struct kvm_vcpu *v, uint32_t request)
{
    v->requests |= request;
    kicks++;
}

int kvm_vcpu_test_clear(struct kvm_vcpu *v, uint32_t request)
{
    int set = (v->requests & request) != 0;

    v->requests &= ~request;
    return set;
}

/** The registers an INIT gives, as vcpu.c does, keeping the APIC base. */
void kvm_vcpu_init_reset(struct kvm_vcpu *v)
{
    uint64_t base = v->sregs.apic_base;

    init_resets++;
    k_memset(&v->regs, 0, sizeof(v->regs));
    k_memset(&v->sregs, 0, sizeof(v->sregs));
    k_memset(&v->events, 0, sizeof(v->events));
    v->regs.rip = 0xFFF0;
    v->sregs.cs.selector = 0xF000;
    v->sregs.cs.base = 0xFFFF0000ull;
    v->sregs.apic_base = base;
}

/* The guest's CPUID: leaf 1 with x2APIC and the TSC deadline, and 39
 * physical address bits. */
static struct kvm_cpuid_entry2 leaf1 = {1, 0, 0, 0x306C3, 0, (1u << 21) | (1u << 24), 0, {0}};
static struct kvm_cpuid_entry2 leafx = {0x80000000u, 0, 0, 0x80000008u, 0, 0, 0, {0}};
static struct kvm_cpuid_entry2 leafx8 = {0x80000008u, 0, 0, 0x3027, 0, 0, 0, {0}};

const struct kvm_cpuid_entry2 *kvm_cpuid_find(const struct kvm_vcpu *v, uint32_t function,
                                              uint32_t index)
{
    (void)v;
    (void)index;
    return function == 1             ? &leaf1
           : function == 0x80000000u ? &leafx
           : function == 0x80000008u ? &leafx8
                                     : NULL;
}

/* The backend: the answers of MMIO reads it is given. */
static int completes;

/** The backend's answer to a read: counted. */
static void fake_complete(struct kvm_vcpu *v)
{
    (void)v;
    completes++;
}

static struct kvm_backend fake;
const struct kvm_backend *kvm_backend;

/* The VM and its vCPUs. */
#define NVCPU 4
static struct kvm_vm vm;
static struct kvm_vcpu *vcpu[NVCPU];
static struct kvm_run runs[NVCPU];

#define BASE 0xFEE00000ull

/** A VM with the split controller, its I/O APIC on 24 pins, and NVCPU
 * vCPUs at reset, vCPU 0 the boot one. */
static void vm_make(int full)
{
    struct kvm_enable_cap cap;
    int i;

    k_memset(&vm, 0, sizeof(vm));
    vm.apic_bus_ns = KVM_APIC_BUS_NS_DEFAULT;
    k_memset(&cap, 0, sizeof(cap));
    cap.cap = KVM_CAP_SPLIT_IRQCHIP;
    cap.args[0] = 24;
    CHECK((full ? kvm_irq_ioctl(&vm, KVM_CREATE_IRQCHIP, 0) : kvm_irq_enable_split(&vm, &cap)) == 0,
          "the requested controller");
    for (i = 0; i < NVCPU; i++) {
        struct kvm_vcpu *v = kzalloc(sizeof(*v));

        v->vm = &vm;
        v->id = (uint32_t)i;
        v->index = (uint32_t)i;
        v->tsc_khz = 2000000u;
        v->run = &runs[i];
        v->sregs.apic_base = BASE | APIC_BASE_ENABLE | (i == 0 ? APIC_BASE_BSP : 0);
        v->mp_state = KVM_MP_STATE_RUNNABLE;
        CHECK(kvm_lapic_create(v) == 0, "a vCPU's local APIC");
        vm.vcpus[i] = v;
        vm.nr_vcpus++;
        vcpu[i] = v;
    }
}

/** Free the VM. */
static void vm_end(void)
{
    int i;

    kvm_irq_free(&vm);
    kvm_iobus_free(&vm);
    for (i = 0; i < NVCPU; i++) {
        kvm_lapic_free(vcpu[i]);
        kfree(vcpu[i]);
    }
}

/** An MMIO access of `len` bytes at `off` in the APIC page of `v`: the
 * value read, the bytes in order. */
static uint32_t mmio(struct kvm_vcpu *v, uint32_t off, uint32_t len, int write, uint32_t value)
{
    uint32_t i, r = 0;

    k_memset(&v->exit, 0, sizeof(v->exit));
    v->exit.kind = KVM_X_MMIO;
    v->exit.mmio.phys_addr = (v->sregs.apic_base & APIC_BASE_ADDR) + off;
    v->exit.mmio.len = len;
    v->exit.mmio.is_write = (uint8_t)write;
    for (i = 0; i < 4u; i++) {
        v->exit.mmio.data[i] = (uint8_t)(value >> (i * 8u));
    }
    CHECK(kvm_lapic_mmio(v) == 1, "an access to the APIC page is the APIC's");
    for (i = 0; i < len && i < 4u; i++) {
        r |= (uint32_t)v->exit.mmio.data[i] << (i * 8u);
    }
    return r;
}

/** Write the register at `off` of `v` through MMIO. */
static void wr(struct kvm_vcpu *v, uint32_t off, uint32_t value)
{
    mmio(v, off, 4, 1, value);
}

/** Read the register at `off` of `v` through MMIO. */
static uint32_t rd(struct kvm_vcpu *v, uint32_t off)
{
    return mmio(v, off, 4, 0, 0);
}

/** Whether `vector` is set in the vector register at `off` of `v`. */
static int has(struct kvm_vcpu *v, uint32_t off, uint32_t vector)
{
    return (apic_of(v)->regs[(off >> 2) + (vector >> 5) * 4u] >> (vector & 31u)) & 1u;
}

/** Clear IRR, ISR and TMR of every vCPU. */
static void clear_vectors(void)
{
    int i;
    uint32_t w;

    for (i = 0; i < NVCPU; i++) {
        for (w = 0; w < 8u; w++) {
            apic_of(vcpu[i])->regs[(APIC_IRR >> 2) + w * 4u] = 0;
            apic_of(vcpu[i])->regs[(APIC_ISR >> 2) + w * 4u] = 0;
            apic_of(vcpu[i])->regs[(APIC_TMR >> 2) + w * 4u] = 0;
        }
        update_ppr(apic_of(vcpu[i]));
    }
}

/** An MSI's address for `dest`, logical or physical. */
static uint32_t msi_addr(uint32_t dest, int logical)
{
    return 0xFEE00000u | (dest << 12) | (logical ? 4u : 0u);
}

/** An MSI's data: the vector, the delivery mode, level-triggered or not. */
static uint32_t msi_data(uint32_t vector, uint32_t mode, int level)
{
    return vector | mode | (level ? 0xC000u : 0u);
}

/** KVM_SIGNAL_MSI of `addr` and `data`. */
static int32_t signal(uint32_t addr, uint32_t data)
{
    struct kvm_msi m;

    k_memset(&m, 0, sizeof(m));
    m.address_lo = addr;
    m.data = data;
    return kvm_irq_ioctl(&vm, KVM_SIGNAL_MSI, (uintptr_t)&m);
}

/** Read the x2APIC MSR `index` of `v`: the value, or ~0 for #GP. */
static uint64_t msr_rd(struct kvm_vcpu *v, uint32_t index)
{
    uint64_t value = 0;

    return kvm_lapic_msr(v, index, 0, &value, 0) == 0 ? value : ~0ull;
}

/** Write the MSR `index` of `v` from the guest: 0 or -1 for #GP. */
static int msr_wr(struct kvm_vcpu *v, uint32_t index, uint64_t value)
{
    return kvm_lapic_msr(v, index, 1, &value, 0);
}

/** Enable every vCPU's APIC in SPIV, with spurious vector 0xFF. */
static void enable_all(void)
{
    int i;

    for (i = 0; i < NVCPU; i++) {
        wr(vcpu[i], APIC_SPIV, 0x1FF);
    }
}

/** The reset state and the page's reads. */
static void reset_rows(void)
{
    struct kvm_vcpu *v = vcpu[1];

    CHECK(rd(v, APIC_ID) == 1u << 24, "the xAPIC id is the vCPU's in bits 31:24");
    CHECK(rd(v, APIC_LVR) == 0x01050014u, "version 0x14, six LVT entries, directed EOI");
    CHECK(rd(v, APIC_SPIV) == 0xFF && rd(v, APIC_DFR) == 0xFFFFFFFFu,
          "SPIV and DFR at reset, the APIC disabled in software");
    CHECK(rd(v, APIC_LVTT) == APIC_LVT_MASKED && rd(v, APIC_LVT0) == APIC_LVT_MASKED &&
              rd(v, APIC_LVTERR) == APIC_LVT_MASKED,
          "every LVT entry masked at reset");
    CHECK(rd(vcpu[0], APIC_LVT0) == KVM_APIC_DM_EXTINT, "the boot vCPU's LINT0 takes ExtINT");
    completes = 0;
    CHECK(mmio(v, APIC_ID + 3, 1, 0, 0) == 1, "a byte read of the id");
    CHECK(completes == 1, "a read is answered to the backend");
    CHECK(mmio(v, APIC_ID + 2, 4, 0, 0) == 0, "a read across registers reads nothing");
    CHECK(rd(v, APIC_EOI) == 0 && rd(v, APIC_ARBPRI) == 0, "EOI is write-only, ARBPRI reads 0");
    CHECK(rd(v, 0x3F0) == 0, "SELF_IPI is not an xAPIC register");
    v->exit.mmio.phys_addr = BASE + 0x1000;
    v->exit.kind = KVM_X_MMIO;
    CHECK(kvm_lapic_mmio(v) == 0, "an address past the page goes on");
    CHECK(signal(msi_addr(1, 0), msi_data(0x40, 0, 0)) == 0,
          "an APIC disabled in software takes no fixed interrupt");
    CHECK(!has(v, APIC_IRR, 0x40), "nothing in IRR");
}

/** Priorities, acknowledgement and EOI. */
static void priority_rows(void)
{
    struct kvm_vcpu *v = vcpu[1];

    enable_all();
    clear_vectors();
    wr(v, APIC_TASKPRI, 0x20);
    CHECK(rd(v, APIC_PROCPRI) == 0x20, "PPR is TPR with nothing in service");
    CHECK(signal(msi_addr(1, 0), msi_data(0x45, 0, 0)) == 1, "a fixed MSI is taken");
    CHECK(has(v, APIC_IRR, 0x45) && !has(v, APIC_TMR, 0x45), "in IRR, edge-triggered");
    CHECK(kvm_lapic_pending(v) == 0x45, "a vector above PPR's class is pending");
    wr(v, APIC_TASKPRI, 0x40);
    CHECK(kvm_lapic_pending(v) == -1, "a vector of TPR's class is held");
    wr(v, APIC_TASKPRI, 0x30);
    CHECK(kvm_lapic_ack(v) == 0x45, "the acknowledgement takes it");
    CHECK(has(v, APIC_ISR, 0x45) && !has(v, APIC_IRR, 0x45), "from IRR into ISR");
    CHECK(rd(v, APIC_PROCPRI) == 0x40, "PPR is the class in service above TPR");
    signal(msi_addr(1, 0), msi_data(0x41, 0, 0));
    CHECK(kvm_lapic_pending(v) == -1, "a vector of the class in service waits");
    signal(msi_addr(1, 0), msi_data(0x61, 0, 1));
    CHECK(has(v, APIC_TMR, 0x61), "a level-triggered one sets TMR");
    CHECK(kvm_lapic_pending(v) == 0x61, "a higher class is pending over one in service");
    kvm_lapic_ack(v);
    wr(v, APIC_EOI, 0);
    CHECK(!has(v, APIC_ISR, 0x61) && has(v, APIC_ISR, 0x45), "EOI ends the highest in service");
    wr(v, APIC_EOI, 0);
    CHECK(kvm_lapic_pending(v) == 0x41, "after both EOIs the held vector is pending");
    wr(v, APIC_TASKPRI, 0);
    CHECK(signal(msi_addr(1, 0), msi_data(0x50, 0, 1) & ~0x4000u) == 1,
          "an MSI's assert bit is ignored, as Linux's kvm_set_msi_irq");
}

/**
 * The parts a backend's TPR shadow uses, as Linux's lapic.c gives them:
 * the register page, page-aligned at the physical address the backend
 * hands the processor, and 0 with no APIC in the kernel; the highest
 * vector in IRR, which SPIV's enable bit does not hide, as
 * kvm_lapic_find_highest_irr (lapic.c:830-838) checks no enable, and -1
 * with the APIC off in IA32_APIC_BASE; and the PPR update after the
 * guest lowered TPR on the page, which asks for an injection, as
 * handle_tpr_below_threshold calls kvm_apic_update_ppr (vmx.c:5653-5657).
 */
static void tpr_shadow_rows(void)
{
    struct kvm_vcpu *v = vcpu[1];
    struct kvm_vcpu bare;
    struct lapic *l = apic_of(v);

    k_memset(&bare, 0, sizeof(bare));
    bare.vm = &vm;
    CHECK(kvm_lapic_page(v) == l->regs_pa && l->regs_pa != 0 && (l->regs_pa & 0xFFFu) == 0 &&
              (uintptr_t)l->regs == l->regs_pa && kvm_lapic_page(vcpu[2]) != l->regs_pa,
          "each APIC's register page is a page of its own, at the address the backend is given");
    CHECK(kvm_lapic_page(&bare) == 0 && kvm_lapic_highest_irr(&bare) == -1,
          "with no APIC in the kernel there is no page and no vector");

    enable_all();
    clear_vectors();
    CHECK(kvm_lapic_highest_irr(v) == -1, "nothing in IRR is -1");
    wr(v, APIC_TASKPRI, 0x60);
    signal(msi_addr(1, 0), msi_data(0x51, 0, 0));
    signal(msi_addr(1, 0), msi_data(0x32, 0, 0));
    CHECK(kvm_lapic_highest_irr(v) == 0x51 && kvm_lapic_pending(v) == -1,
          "the highest vector in IRR, 0x51, which TPR 0x60 holds back");
    wr(v, APIC_SPIV, 0xFF);
    CHECK(kvm_lapic_highest_irr(v) == 0x51,
          "SPIV's enable bit off, the vector still waits in IRR and is still the highest");

    v->requests = 0;
    l->regs[APIC_TASKPRI >> 2] = 0x40;
    CHECK(reg_get(l, APIC_PROCPRI) == 0x60 && !(v->requests & KVM_REQ_EVENT),
          "TPR lowered on the page, as the processor writes it, leaves PPR until the update");
    kvm_lapic_update_ppr(v);
    CHECK(reg_get(l, APIC_PROCPRI) == 0x40 && (v->requests & KVM_REQ_EVENT),
          "the update lowers PPR to TPR 0x40 and asks for an injection");
    CHECK(kvm_lapic_pending(v) == 0x51, "and the vector is pending, with SPIV's enable bit off");

    CHECK(kvm_lapic_set_base(v, BASE, 0) == 0 && kvm_lapic_highest_irr(v) == -1,
          "the APIC off in IA32_APIC_BASE gives -1, with the vector still in IRR");
    CHECK(kvm_lapic_set_base(v, BASE | APIC_BASE_ENABLE, 0) == 0 &&
              kvm_lapic_highest_irr(v) == 0x51,
          "and back on, the vector again");
    enable_all();
    wr(v, APIC_TASKPRI, 0);
    clear_vectors();
    v->requests = 0;
}

/** Destinations and delivery modes. */
static void delivery_rows(void)
{
    int i;

    clear_vectors();
    CHECK(signal(msi_addr(0xFF, 0), msi_data(0x52, 0, 0)) == NVCPU, "a physical broadcast");
    CHECK(signal(msi_addr(7, 0), msi_data(0x52, 0, 0)) == -EPERM,
          "an MSI to no APIC is Linux's -1");
    wr(vcpu[1], APIC_LDR, 0x02000000);
    wr(vcpu[2], APIC_LDR, 0x04000000);
    wr(vcpu[3], APIC_LDR, 0x21000000);
    wr(vcpu[3], APIC_DFR, 0x0FFFFFFF);
    clear_vectors();
    CHECK(signal(msi_addr(0x06, 1), msi_data(0x53, 0, 0)) == 2, "a flat logical destination");
    CHECK(has(vcpu[1], APIC_IRR, 0x53) && has(vcpu[2], APIC_IRR, 0x53) &&
              !has(vcpu[0], APIC_IRR, 0x53),
          "both vCPUs of the flat mask");
    CHECK(signal(msi_addr(0x21, 1), msi_data(0x54, 0, 0)) == 1 && has(vcpu[3], APIC_IRR, 0x54),
          "a cluster destination");
    CHECK(signal(msi_addr(0x11, 1), msi_data(0x54, 0, 0)) == -EPERM, "another cluster");
    clear_vectors();
    signal(msi_addr(0x06, 1), msi_data(0x51, KVM_APIC_DM_LOWEST, 0));
    signal(msi_addr(0x06, 1), msi_data(0x50, KVM_APIC_DM_LOWEST, 0));
    CHECK(has(vcpu[2], APIC_IRR, 0x51) && !has(vcpu[1], APIC_IRR, 0x51),
          "lowest priority: vector 0x51 to the second of two");
    CHECK(has(vcpu[1], APIC_IRR, 0x50) && !has(vcpu[2], APIC_IRR, 0x50),
          "lowest priority: vector 0x50 to the first");
    clear_vectors();
    signal(msi_addr(0x06, 1) | 0x8u, msi_data(0x51, 0, 0));
    CHECK(has(vcpu[2], APIC_IRR, 0x51) && !has(vcpu[1], APIC_IRR, 0x51),
          "the redirection hint makes a fixed MSI go to one");
    clear_vectors();
    wr(vcpu[0], APIC_ICR2, 2u << 24);
    wr(vcpu[0], APIC_ICR, 0x4000 | 0x56);
    CHECK(has(vcpu[2], APIC_IRR, 0x56) && !has(vcpu[1], APIC_IRR, 0x56), "a physical IPI");
    CHECK(!(rd(vcpu[0], APIC_ICR) & APIC_SEND_PENDING), "the ICR never reads busy");
    wr(vcpu[0], APIC_ICR, 0x4000 | 0xC0000 | 0x57);
    CHECK(!has(vcpu[0], APIC_IRR, 0x57), "all but self skips the sender");
    for (i = 1; i < NVCPU; i++) {
        CHECK(has(vcpu[i], APIC_IRR, 0x57), "all but self reaches the others");
    }
    wr(vcpu[0], APIC_ICR, 0x4000 | 0x40000 | 0x58);
    CHECK(has(vcpu[0], APIC_IRR, 0x58) && !has(vcpu[1], APIC_IRR, 0x58), "a self IPI");
    vcpu[2]->nmi_queued = 0;
    wr(vcpu[0], APIC_ICR, 0x4000 | 0x400);
    CHECK(kvm_lapic_prepare(vcpu[2]) == 0 && vcpu[2]->nmi_queued == 1, "an NMI is queued");
    clear_vectors();
}

/** INIT and SIPI. */
static void init_rows(void)
{
    struct kvm_vcpu *ap = vcpu[1];
    struct kvm_mp_state mp;

    ap->requests = 0;
    wr(vcpu[0], APIC_ICR2, 1u << 24);
    wr(vcpu[0], APIC_ICR, 0x4500);
    CHECK(apic_of(ap)->events == EV_INIT, "an INIT is left for the vCPU");
    init_resets = 0;
    waits = 0;
    endless = 0;
    sleep_to_tick = 0;
    caller.sig_woken = 1;
    CHECK(kvm_lapic_prepare(ap) == -EINTR && ap->run->exit_reason == KVM_EXIT_INTR && endless == 0,
          "a vCPU waiting for SIPI blocks until a signal");
    caller.sig_woken = 0;
    CHECK(init_resets == 1 && ap->mp_state == KVM_MP_STATE_INIT_RECEIVED && waits == 1,
          "the INIT resets an AP to wait for SIPI");
    CHECK(rd(ap, APIC_SPIV) == 0xFF && rd(ap, APIC_ID) == 1u << 24,
          "the INIT resets the APIC and keeps its id");
    wr(ap, APIC_SPIV, 0x1FF);
    wr(vcpu[0], APIC_ICR, 0x4600 | 0x9A);
    CHECK(kvm_lapic_prepare(ap) == 0 && ap->mp_state == KVM_MP_STATE_RUNNABLE,
          "the SIPI starts it");
    CHECK(ap->sregs.cs.selector == 0x9A00 && ap->sregs.cs.base == 0x9A000 && ap->regs.rip == 0,
          "at the vector's page in real mode");
    wr(vcpu[0], APIC_ICR, 0x4600 | 0x10);
    kvm_lapic_prepare(ap);
    CHECK(ap->sregs.cs.selector == 0x9A00, "a SIPI to a running vCPU is dropped");
    wr(vcpu[0], APIC_ICR, 0x4500);
    wr(vcpu[0], APIC_ICR, 0x4600 | 0x9B);
    CHECK(apic_of(ap)->events == (EV_INIT | EV_SIPI),
          "an INIT and the SIPI after it wait together for a vCPU that has not looked");
    CHECK(kvm_lapic_prepare(ap) == 0 && ap->mp_state == KVM_MP_STATE_RUNNABLE &&
              ap->sregs.cs.selector == 0x9B00 && ap->sregs.cs.base == 0x9B000 && ap->regs.rip == 0,
          "and the INIT's reset of the vCPU keeps the SIPI's vector");
    wr(ap, APIC_SPIV, 0x1FF);
    wr(vcpu[0], APIC_ICR, 0x40000 | 0x4500);
    kvm_lapic_prepare(vcpu[0]);
    CHECK(vcpu[0]->mp_state == KVM_MP_STATE_RUNNABLE, "an INIT leaves the boot vCPU running");
    wr(vcpu[0], APIC_SPIV, 0x1FF);
    ap->events.sipi_vector = 0x10;
    mp.mp_state = KVM_MP_STATE_SIPI_RECEIVED;
    CHECK(kvm_lapic_ioctl(ap, KVM_SET_MP_STATE, (uintptr_t)&mp) == 0,
          "KVM_SET_MP_STATE of SIPI_RECEIVED");
    CHECK(ap->mp_state == KVM_MP_STATE_INIT_RECEIVED, "waits for SIPI with one pending");
    ap->events.sipi_vector = 0x30;
    CHECK(kvm_lapic_ioctl(ap, KVM_GET_MP_STATE, (uintptr_t)&mp) == 0 &&
              mp.mp_state == KVM_MP_STATE_RUNNABLE && ap->sregs.cs.selector == 0x3000,
          "KVM_GET_MP_STATE takes the SIPI first, at the vector the events hold by then");
    mp.mp_state = 5;
    CHECK(kvm_lapic_ioctl(ap, KVM_SET_MP_STATE, (uintptr_t)&mp) == -EINVAL,
          "an MP state of another architecture");
}

/** The timer in its three modes. */
static void timer_rows(void)
{
    struct kvm_vcpu *v = vcpu[2];
    struct lapic *l = apic_of(v);
    uint64_t tsc;

    clear_vectors();
    CHECK(l->divide == 2, "TDCR at reset divides by 2");
    wr(v, APIC_TDCR, 0xB);
    CHECK(l->divide == 1, "TDCR 0xB divides by 1");
    wr(v, APIC_LVTT, 0xEF);
    wr(v, APIC_TMICT, 1000);
    mono_ns += 400;
    CHECK(rd(v, APIC_TMCCT) == 600, "TMCCT counts down at the 1 GHz bus");
    timer_check(l);
    CHECK(!has(v, APIC_IRR, 0xEF), "not before the count runs out");
    mono_ns += 600;
    timer_check(l);
    CHECK(has(v, APIC_IRR, 0xEF), "a one-shot expiry delivers LVTT's vector");
    CHECK(rd(v, APIC_TMCCT) == 0 && rd(v, APIC_TMICT) == 1000, "and stops, TMICT kept");
    clear_vectors();
    wr(v, APIC_LVTT, 0x200EE);
    wr(v, APIC_TMICT, 100);
    CHECK(l->period_ns == MIN_PERIOD_NS, "a periodic count is no shorter than 200 us");
    mono_ns += 450000;
    timer_check(l);
    CHECK(has(v, APIC_IRR, 0xEE), "a periodic expiry");
    CHECK(rd(v, APIC_TMCCT) == 150000, "missed periods are one, the count goes on");
    wr(v, APIC_TDCR, 0x0);
    CHECK(l->divide == 2 && l->period_ns == MIN_PERIOD_NS, "a new divisor");
    wr(v, APIC_TMICT, 0);
    CHECK(l->expire_ns == 0, "TMICT 0 stops the count");
    clear_vectors();
    wr(v, APIC_LVTT, 0x400ED);
    CHECK(l->timer_mode == TIMER_DEADLINE, "TSC-deadline mode");
    wr(v, APIC_TMICT, 500);
    CHECK(rd(v, APIC_TMICT) == 0, "TMICT is ignored in TSC-deadline mode");
    tsc = arch_cycles() + v->tsc_offset;
    CHECK(msr_wr(v, MSR_IA32_TSC_DEADLINE, tsc + 1000) == 0 &&
              msr_rd(v, MSR_IA32_TSC_DEADLINE) == tsc + 1000,
          "IA32_TSC_DEADLINE written and read");
    mono_ns += 499;
    timer_check(l);
    CHECK(!has(v, APIC_IRR, 0xED), "not before the deadline");
    mono_ns += 1;
    timer_check(l);
    CHECK(has(v, APIC_IRR, 0xED) && msr_rd(v, MSR_IA32_TSC_DEADLINE) == 0,
          "the deadline delivers and disarms");
    leaf1.ecx &= ~(1u << 24);
    wr(v, APIC_LVTT, 0x400ED);
    CHECK(l->timer_mode == TIMER_ONESHOT && rd(v, APIC_LVTT) == 0xED,
          "no TSC-deadline mode without the CPUID bit");
    leaf1.ecx |= 1u << 24;
    wr(v, APIC_LVTT, 0xEC);
    wr(v, APIC_SPIV, 0xFF);
    CHECK(rd(v, APIC_LVTT) & APIC_LVT_MASKED, "disabling in SPIV masks the LVT");
    wr(v, APIC_LVT1, 0x400);
    CHECK(rd(v, APIC_LVT1) == (APIC_LVT_MASKED | 0x400), "an LVT write stays masked meanwhile");
    wr(v, APIC_SPIV, 0x1FF);
    clear_vectors();
}

/** The local APIC's two quirks turned off, as KVM_CAP_DISABLE_QUIRKS2
 * turns them, and a bus cycle KVM_CAP_X86_APIC_BUS_CYCLES_NS set; the VM
 * ends after these rows. */
static void quirk_rows(void)
{
    struct kvm_vcpu *v = vcpu[0];
    struct lapic *l = apic_of(v);

    vm.disabled_quirks = KVM_X86_QUIRK_LINT0_REENABLED | KVM_X86_QUIRK_LAPIC_MMIO_HOLE;
    lapic_reset(l, 0);
    CHECK(rd(v, APIC_LVT0) == APIC_LVT_MASKED,
          "with LINT0's quirk off the boot vCPU's LINT0 starts masked");
    v->sregs.apic_base &= ~(uint64_t)APIC_BASE_ENABLE;
    k_memset(&v->exit, 0, sizeof(v->exit));
    v->exit.kind = KVM_X_MMIO;
    v->exit.mmio.phys_addr = BASE + APIC_ID;
    v->exit.mmio.len = 4;
    CHECK(kvm_lapic_mmio(v) == 0,
          "with the MMIO hole's quirk off, the page of an APIC turned off is the caller's");
    v->sregs.apic_base |= APIC_BASE_ENABLE;
    vm.disabled_quirks = 0;

    vm.apic_bus_ns = 4;
    wr(v, APIC_SPIV, 0x1FF);
    wr(v, APIC_TDCR, 0xB);
    wr(v, APIC_LVTT, 0xEB);
    wr(v, APIC_TMICT, 1000);
    mono_ns += 400;
    CHECK(rd(v, APIC_TMCCT) == 900, "at a bus cycle of 4 ns, 400 ns count 100 off");
    mono_ns += 3599;
    timer_check(l);
    CHECK(!has(v, APIC_IRR, 0xEB), "and the count of 1000 has not run out after 3999 ns");
    mono_ns += 1;
    timer_check(l);
    CHECK(has(v, APIC_IRR, 0xEB), "but has after 4000");
    vm.apic_bus_ns = KVM_APIC_BUS_NS_DEFAULT;
    clear_vectors();
}

/** x2APIC mode. */
static void x2apic_rows(void)
{
    struct kvm_vcpu *v = vcpu[2];

    CHECK(msr_rd(v, 0x802) == ~0ull, "an x2APIC MSR in xAPIC mode is #GP");
    CHECK(kvm_lapic_set_base(v, BASE | APIC_BASE_ENABLE | APIC_BASE_X2APIC, 0) == 0,
          "into x2APIC mode");
    CHECK(msr_rd(v, 0x802) == 2, "the x2APIC id is the vCPU's");
    CHECK(msr_rd(v, 0x80D) == 4, "LDR is cluster 0, bit 2");
    CHECK(msr_rd(v, 0x803) == 0x01050014u, "the version through its MSR");
    CHECK(mmio(v, APIC_ID, 4, 0, 0) == 0xFFFFFFFFu, "MMIO reads all ones in x2APIC mode");
    mmio(v, APIC_TASKPRI, 4, 1, 0x70);
    CHECK(msr_rd(v, 0x808) == 0, "an MMIO write is dropped");
    CHECK(msr_rd(v, 0x80E) == ~0ull && msr_rd(v, 0x831) == ~0ull && msr_rd(v, 0x809) == ~0ull,
          "DFR, ICR2 and ARBPRI are not x2APIC registers");
    CHECK(msr_rd(v, 0x83F) == ~0ull, "SELF_IPI is write-only");
    CHECK(msr_wr(v, 0x802, 5) == -1 && msr_wr(v, 0x80D, 1) == -1, "the id and LDR are read-only");
    CHECK(msr_wr(v, 0x808, 1ull << 32) == -1, "the high half of a register is reserved");
    CHECK(msr_wr(v, 0x828, 1) == -1 && msr_wr(v, 0x828, 0) == 0, "ESR takes only 0");
    CHECK(msr_wr(v, 0x830, 0x2000 | 0x40) == -1, "a reserved ICR bit is #GP");
    CHECK(msr_wr(v, 0x83F, 0x77) == 0 && has(v, APIC_IRR, 0x77), "a self IPI by its MSR");
    CHECK(msr_wr(v, 0x83F, 0x177) == -1, "SELF_IPI takes the vector alone");
    CHECK(msr_wr(v, 0x830, (1ull << 32) | 0x4000 | 0x66) == 0 && has(vcpu[1], APIC_IRR, 0x66),
          "an IPI with the destination in the ICR's high half");
    CHECK(msr_rd(v, 0x830) == ((1ull << 32) | 0x4000 | 0x66), "the ICR reads 64 bits");
    CHECK(msr_wr(v, 0x80B, 0) == 0, "EOI by its MSR");
    CHECK(kvm_lapic_set_base(v, BASE | APIC_BASE_ENABLE, 0) == -1,
          "the guest may not go back to xAPIC");
    CHECK(kvm_lapic_set_base(v, BASE | APIC_BASE_X2APIC, 0) == -1, "x2APIC without the enable");
    CHECK(kvm_lapic_set_base(v, BASE | (1ull << 40), 1) == -1, "an address past MAXPHYADDR");
    CHECK(kvm_lapic_set_base(v, BASE, 0) == 0, "disabled from x2APIC");
    CHECK(kvm_lapic_set_base(v, BASE | APIC_BASE_ENABLE | APIC_BASE_X2APIC, 0) == -1,
          "the guest may not go from disabled to x2APIC");
    CHECK(kvm_lapic_set_base(v, BASE | APIC_BASE_ENABLE | APIC_BASE_X2APIC, 1) == 0,
          "the host may");
    CHECK(kvm_lapic_msr(v, 0x10, 0, NULL, 0) == 1, "another MSR is not the APIC's");
    leaf1.ecx &= ~(1u << 21);
    CHECK(kvm_lapic_set_base(vcpu[3], BASE | APIC_BASE_ENABLE | APIC_BASE_X2APIC, 1) == -1,
          "no x2APIC without the CPUID bit");
    leaf1.ecx |= 1u << 21;
    clear_vectors();
}

/** KVM_GET_LAPIC and KVM_SET_LAPIC. */
static void state_rows(void)
{
    struct kvm_lapic_state s;
    struct kvm_vcpu *v = vcpu[2];
    uint32_t id;

    CHECK(kvm_lapic_ioctl(v, KVM_GET_LAPIC, (uintptr_t)&s) == 0, "KVM_GET_LAPIC");
    k_memcpy(&id, s.regs + APIC_ID, 4);
    CHECK(id == 2u << 24, "the x2APIC id in xAPIC's place");
    apic_of(v)->regs[APIC_LDR >> 2] = 0;
    CHECK(kvm_lapic_ioctl(v, KVM_SET_LAPIC, (uintptr_t)&s) == 0, "KVM_SET_LAPIC");
    CHECK(msr_rd(v, 0x802) == 2 && msr_rd(v, 0x80D) == 4, "the id and LDR restored in x2APIC");
    v = vcpu[3];
    wr(v, APIC_TDCR, 0xB);
    wr(v, APIC_LVTT, 0xE0);
    wr(v, APIC_TMICT, 1000);
    mono_ns += 300;
    kvm_lapic_ioctl(v, KVM_GET_LAPIC, (uintptr_t)&s);
    k_memcpy(&id, s.regs + APIC_TMCCT, 4);
    CHECK(id == 700, "TMCCT saved as the guest reads it");
    wr(v, APIC_TMICT, 0);
    mono_ns += 5000;
    kvm_lapic_ioctl(v, KVM_SET_LAPIC, (uintptr_t)&s);
    CHECK(rd(v, APIC_TMCCT) == 700 && rd(v, APIC_TMICT) == 1000,
          "the count restarts from the TMCCT restored");
    CHECK(kvm_lapic_ioctl(v, KVM_GET_LAPIC, 0) == -EFAULT, "a bad address");
    wr(v, APIC_TMICT, 0);
}

/** KVM_INTERRUPT's external vector and the window. */
static void extint_rows(void)
{
    struct kvm_vcpu *v = vcpu[0];
    struct kvm_interrupt irq;

    clear_vectors();
    irq.irq = 0x30;
    CHECK(kvm_lapic_ioctl(v, KVM_INTERRUPT, (uintptr_t)&irq) == 0, "KVM_INTERRUPT");
    CHECK(kvm_lapic_ioctl(v, KVM_INTERRUPT, (uintptr_t)&irq) == -EEXIST, "one at a time");
    v->irq_allowed = 1;
    CHECK(!kvm_lapic_ready(v), "not ready while one waits");
    CHECK(kvm_lapic_pending(v) == 0x30 && kvm_lapic_ack(v) == 0x30, "ExtINT through LINT0");
    CHECK(kvm_lapic_ready(v), "ready once taken");
    CHECK(!has(v, APIC_ISR, 0x30), "an ExtINT is not in service at the APIC");
    irq.irq = 256;
    CHECK(kvm_lapic_ioctl(v, KVM_INTERRUPT, (uintptr_t)&irq) == -EINVAL, "a vector past 255");
    irq.irq = 0x31;
    kvm_lapic_ioctl(vcpu[1], KVM_INTERRUPT, (uintptr_t)&irq);
    CHECK(kvm_lapic_pending(vcpu[1]) == -1, "LINT0 masked takes no ExtINT");
    apic_of(vcpu[1])->ext_vector = -1;
}

/** The halted vCPU. */
static void halt_rows(void)
{
    struct kvm_vcpu *v = vcpu[3];
    uint64_t tsc, exits;

    clear_vectors();
    v->requests = 0;
    v->if_flag = 1;
    CHECK(kvm_lapic_halt(v) == 0 && v->mp_state == KVM_MP_STATE_HALTED, "HLT halts in the kernel");
    sleep_to_tick = 0;
    waits = 0;
    endless = 0;
    CHECK(kvm_lapic_prepare(v) == -EINTR && endless == 1 && waits == 1,
          "with the signal-work flag clear, nothing ends the halted wait");
    caller.sig_woken = 1;
    exits = v->stat[KVM_VCPU_STAT_SIGNAL_EXITS];
    CHECK(kvm_lapic_prepare(v) == -EINTR && v->mp_state == KVM_MP_STATE_HALTED && endless == 1 &&
              waits == 2,
          "the flag, which a signal sets, ends the halted wait, the vCPU still halted");
    CHECK(v->stat[KVM_VCPU_STAT_SIGNAL_EXITS] == exits + 1 && v->run->exit_reason == KVM_EXIT_INTR,
          "counted as a signal exit, as Linux's kvm_handle_signal_exit");
    caller.sig_woken = 0;
    signal(msi_addr(3, 0), msi_data(0x90, 0, 0));
    CHECK(kvm_lapic_prepare(v) == 0 && v->mp_state == KVM_MP_STATE_RUNNABLE,
          "an interrupt wakes it");
    CHECK(v->requests & KVM_REQ_EVENT, "and asks for the injection");
    kvm_lapic_ack(v);
    wr(v, APIC_EOI, 0);
    v->if_flag = 0;
    kvm_lapic_halt(v);
    signal(msi_addr(3, 0), msi_data(0x91, 0, 0));
    caller.sig_woken = 1;
    CHECK(kvm_lapic_prepare(v) == -EINTR && endless == 1,
          "with IF clear an interrupt does not wake it");
    caller.sig_woken = 0;
    v->if_flag = 1;
    clear_vectors();
    wr(v, APIC_LVTT, 0x400E8);
    tsc = arch_cycles() + v->tsc_offset;
    msr_wr(v, MSR_IA32_TSC_DEADLINE, tsc + 4u * TICK_NS);
    sleep_to_tick = 1;
    waits = 0;
    CHECK(kvm_lapic_prepare(v) == 0 && v->mp_state == KVM_MP_STATE_RUNNABLE && waits == 1,
          "the timer's tick wakes it");
    CHECK(has(v, APIC_IRR, 0xE8), "with the timer's vector");
    CHECK(caller.wakeup_tick == 0, "the wait's tick is cleared");
    sleep_to_tick = 0;
    clear_vectors();
}

/* The routes routing_rows and irqfd_rows set. */
static struct {
    struct kvm_irq_routing h;
    struct kvm_irq_routing_entry e[4];
} rt;

/** Set the route of `gsi` in slot `i` of `rt`. */
static void route(int i, uint32_t gsi, uint32_t addr, uint32_t data)
{
    k_memset(&rt.e[i], 0, sizeof(rt.e[i]));
    rt.e[i].gsi = gsi;
    rt.e[i].type = KVM_IRQ_ROUTING_MSI;
    rt.e[i].u.msi.address_lo = addr;
    rt.e[i].u.msi.data = data;
}

/** KVM_SIGNAL_MSI of `addr` and `data` with the high address `hi`. */
static int32_t signal_hi(uint32_t addr, uint32_t hi, uint32_t data)
{
    struct kvm_msi m;

    k_memset(&m, 0, sizeof(m));
    m.address_lo = addr;
    m.address_hi = hi;
    m.data = data;
    return kvm_irq_ioctl(&vm, KVM_SIGNAL_MSI, (uintptr_t)&m);
}

/** KVM_CAP_X2APIC_API, as Linux's xapic_state_test and its MSI rules:
 * the x2APIC id whole in KVM_GET_LAPIC and checked by KVM_SET_LAPIC, an
 * MSI's destination in the high address, and 0xFF no longer a broadcast
 * to an x2APIC; vCPU 2 is in x2APIC mode. */
static void x2apic_api_rows(void)
{
    struct kvm_lapic_state s;
    struct kvm_vcpu *v = vcpu[2];
    uint32_t id;

    vm.x2apic_format = 1;
    CHECK(kvm_lapic_ioctl(v, KVM_GET_LAPIC, (uintptr_t)&s) == 0, "KVM_GET_LAPIC with 32-bit ids");
    k_memcpy(&id, s.regs + APIC_ID, 4);
    CHECK(id == 2, "the x2APIC id unshifted");
    CHECK(kvm_lapic_ioctl(v, KVM_SET_LAPIC, (uintptr_t)&s) == 0, "KVM_SET_LAPIC of the vCPU's id");
    id = 3;
    k_memcpy(s.regs + APIC_ID, &id, 4);
    CHECK(kvm_lapic_ioctl(v, KVM_SET_LAPIC, (uintptr_t)&s) == -EINVAL && msr_rd(v, 0x802) == 2,
          "and of another id is -EINVAL");
    vm.x2apic_format = 0;
    id = 5u << 24;
    k_memcpy(s.regs + APIC_ID, &id, 4);
    CHECK(kvm_lapic_ioctl(v, KVM_SET_LAPIC, (uintptr_t)&s) == 0 && msr_rd(v, 0x802) == 2 &&
              msr_rd(v, 0x80D) == 4,
          "without 32-bit ids the caller's id is ignored");

    clear_vectors();
    CHECK(signal_hi(msi_addr(2, 0), 0x100, msi_data(0x61, 0, 0)) == 1 && has(v, APIC_IRR, 0x61),
          "without 32-bit ids the high address is ignored");
    vm.x2apic_format = 1;
    CHECK(signal_hi(msi_addr(2, 0), 0x100, msi_data(0x62, 0, 0)) == -EPERM,
          "with them it holds the destination's bits 31:8");
    CHECK(signal_hi(msi_addr(2, 0), 0x1, msi_data(0x62, 0, 0)) == -EINVAL,
          "and its bits 7:0 make the MSI invalid");
    route(0, 9, msi_addr(2, 0), msi_data(0x62, 0, 0));
    rt.e[0].u.msi.address_hi = 0x1;
    rt.h.nr = 1;
    rt.h.flags = 0;
    CHECK(kvm_irq_ioctl(&vm, KVM_SET_GSI_ROUTING, (uintptr_t)&rt) == -EINVAL,
          "a route of such an MSI is -EINVAL");
    vm.x2apic_format = 0;

    clear_vectors();
    CHECK(signal(msi_addr(0xFF, 0), msi_data(0x63, 0, 0)) == NVCPU && has(v, APIC_IRR, 0x63),
          "an MSI to 0xFF is a broadcast to an x2APIC too");
    vm.x2apic_no_broadcast = 1;
    clear_vectors();
    CHECK(signal(msi_addr(0xFF, 0), msi_data(0x63, 0, 0)) == NVCPU - 1 && !has(v, APIC_IRR, 0x63),
          "but not with KVM_X2APIC_API_DISABLE_BROADCAST_QUIRK");
    vm.x2apic_no_broadcast = 0;
    clear_vectors();
}

/** A KVM_IRQ_ROUTING_HV_SINT route of GSI 5 in `rt`'s first slot, raised
 * and lowered through KVM_IRQ_LINE_STATUS. */
static void sint_route_rows(void)
{
    struct kvm_irq_level lvl;

    k_memset(&rt.e[0].u, 0, sizeof(rt.e[0].u));
    rt.e[0].type = KVM_IRQ_ROUTING_HV_SINT;
    rt.e[0].flags = KVM_MSI_VALID_DEVID;
    CHECK(kvm_irq_ioctl(&vm, KVM_SET_GSI_ROUTING, (uintptr_t)&rt) == -EINVAL,
          "a SINT route with a flag");
    rt.e[0].flags = 0;
    rt.e[0].u.hv_sint.vcpu = 2;
    rt.e[0].u.hv_sint.sint = 3;
    CHECK(kvm_irq_ioctl(&vm, KVM_SET_GSI_ROUTING, (uintptr_t)&rt) == 0, "a SINT route");
    sint_raises = 0;
    lvl.irq = 5;
    lvl.level = 1;
    CHECK(kvm_irq_ioctl(&vm, KVM_IRQ_LINE_STATUS, (uintptr_t)&lvl) == 0 && lvl.irq == 1 &&
              sint_raises == 1 && sint_vpidx == 2 && sint_num == 3,
          "a SINT route raises the SINT of its VP index");
    lvl.irq = 5;
    lvl.level = 0;
    kvm_irq_ioctl(&vm, KVM_IRQ_LINE_STATUS, (uintptr_t)&lvl);
    CHECK(lvl.irq == -1 && sint_raises == 1, "a SINT route lowered is -1");
}

/** An MSI route that KVM_CAP_X2APIC_API's 32-bit ids make invalid after
 * KVM_SET_GSI_ROUTING took it: KVM_IRQ_LINE_STATUS on it is -1 and raises
 * nothing, as Linux's kvm_set_irq skips kvm_set_msi's -EINVAL. It leaves
 * that one route in place. */
static void invalid_route_rows(void)
{
    struct kvm_irq_level lvl;

    rt.h.nr = 1;
    rt.h.flags = 0;
    route(0, 9, msi_addr(2, 0), msi_data(0x62, 0, 0));
    rt.e[0].u.msi.address_hi = 0x1;
    CHECK(kvm_irq_ioctl(&vm, KVM_SET_GSI_ROUTING, (uintptr_t)&rt) == 0,
          "an MSI route with high address bits 7:0 set, without 32-bit ids");
    vm.x2apic_format = 1;
    clear_vectors();
    lvl.irq = 9;
    lvl.level = 1;
    CHECK(kvm_irq_ioctl(&vm, KVM_IRQ_LINE_STATUS, (uintptr_t)&lvl) == 0 && lvl.irq == -1 &&
              !has(vcpu[2], APIC_IRR, 0x62),
          "KVM_IRQ_LINE_STATUS on it once 32-bit ids make it invalid is -1, delivered nowhere");
    vm.x2apic_format = 0;
}

/**
 * Have each vCPU take the KVM_REQ_SCAN_IOAPIC that a change of the routes
 * sent it, as Linux's vcpu_enter_guest runs vcpu_scan_ioapic before entry
 * (arch/x86/kvm/x86.c:10390-10391): 1 when every vCPU had the request and
 * its preparation ended with it taken and no exit. The vCPUs' other
 * requests are dropped first, as a run before this one would have taken
 * them.
 */
static int scan_eoi_exits(void)
{
    int i, ok = 1;

    for (i = 0; i < NVCPU; i++) {
        ok = ok && (vcpu[i]->requests & KVM_REQ_SCAN_IOAPIC);
        vcpu[i]->requests &= KVM_REQ_SCAN_IOAPIC;
        ok = ok && kvm_lapic_prepare(vcpu[i]) == 0 && !(vcpu[i]->requests & KVM_REQ_SCAN_IOAPIC);
    }
    return ok;
}

/**
 * The EOI exits of the level route of GSI 5, vector 0x35 to APIC 0, for
 * the vCPU it targets and the others, as Linux's kvm_scan_ioapic_routes
 * marks a vector for a vCPU the route's destination matches or that has
 * the vector pending or in service (arch/x86/kvm/irq_comm.c:405-434), and
 * kvm_ioapic_send_eoi exits only for a marked vector (lapic.c:1463-1482).
 */
static void eoi_exit_rows(void)
{
    struct kvm_vcpu *t = vcpu[0], *o = vcpu[1];

    clear_vectors();
    CHECK(signal(msi_addr(1, 0), msi_data(0x35, 0, 0)) == 1 && kvm_lapic_ack(o) == 0x35,
          "another vCPU takes the route's vector by an edge MSI");
    wr(o, APIC_EOI, 0);
    CHECK(!(o->requests & KVM_REQ_EOI_EXIT),
          "and its EOI makes no exit, since no route targets it");
    CHECK(signal(msi_addr(1, 0), msi_data(0x35, 0, 0)) == 1 && kvm_lapic_ack(o) == 0x35,
          "it takes the vector again");
    CHECK(kvm_irq_ioctl(&vm, KVM_SET_GSI_ROUTING, (uintptr_t)&rt) == 0 && scan_eoi_exits(),
          "the routes set again while the vector is in service there");
    wr(o, APIC_EOI, 0);
    CHECK((o->requests & KVM_REQ_EOI_EXIT) && kvm_lapic_prepare(o) == 1 &&
              o->run->exit_reason == KVM_EXIT_IOAPIC_EOI && o->run->u.eoi.vector == 0x35,
          "a vCPU with the vector in service at the scan exits at its EOI");
    CHECK(kvm_irq_ioctl(&vm, KVM_SET_GSI_ROUTING, (uintptr_t)&rt) == 0 && scan_eoi_exits(),
          "the routes set again once it ended");
    CHECK(signal(msi_addr(1, 0), msi_data(0x35, 0, 0)) == 1 && kvm_lapic_ack(o) == 0x35,
          "the other vCPU takes the vector once more");
    wr(o, APIC_EOI, 0);
    CHECK(!(o->requests & KVM_REQ_EOI_EXIT), "and makes no exit, its mark gone at the scan");
    CHECK(signal(msi_addr(0, 0), msi_data(0x35, 0, 0)) == 1 && kvm_lapic_ack(t) == 0x35,
          "the target takes the vector by an edge MSI");
    wr(t, APIC_EOI, 0);
    CHECK((t->requests & KVM_REQ_EOI_EXIT) && kvm_lapic_prepare(t) == 1 &&
              t->run->u.eoi.vector == 0x35,
          "and exits at its EOI, the vector marked for the vCPU the route targets");
    clear_vectors();
}

/** The split controller, routes, KVM_IRQ_LINE and the EOI exit. */
static void routing_rows(void)
{
    struct kvm_vm none;
    struct kvm_enable_cap cap;
    struct kvm_irq_level lvl;
    struct kvm_vcpu *v = vcpu[0];

    CHECK(kvm_irq_check_extension(KVM_CAP_IRQCHIP) == 1 &&
              kvm_irq_check_extension(KVM_CAP_SPLIT_IRQCHIP) == 1 &&
              kvm_irq_check_extension(KVM_CAP_IRQ_ROUTING) == 4096,
          "the irqchip, the split controller and 4096 routes");
    CHECK(kvm_irq_check_extension(KVM_CAP_TSC_DEADLINE_TIMER) == 1 &&
              kvm_iobus_check_extension(KVM_CAP_IOEVENTFD_NO_LENGTH) == 1,
          "the TSC deadline and ioeventfds of no length");
    k_memset(&none, 0, sizeof(none));
    k_memset(&cap, 0, sizeof(cap));
    cap.args[0] = 4097;
    CHECK(kvm_irq_enable_split(&none, &cap) == -EINVAL, "more pins than routes");
    CHECK(kvm_irq_ioctl(&none, KVM_IRQ_LINE, (uintptr_t)&lvl) == -ENXIO,
          "KVM_IRQ_LINE with no controller");
    CHECK(kvm_irq_ioctl(&none, KVM_SET_GSI_ROUTING, (uintptr_t)&rt) == -EINVAL,
          "routes with no controller");
    cap.args[0] = 0;
    CHECK(kvm_irq_enable_split(&vm, &cap) == -EEXIST, "the controller twice");
    CHECK(kvm_irq_ioctl(&vm, KVM_CREATE_IRQCHIP, 0) == -EEXIST, "KVM_CREATE_IRQCHIP after it");
    CHECK(kvm_irq_ioctl(&vm, 0xAEFF, 0) == -ENOTTY, "another ioctl");

    rt.h.nr = 1;
    rt.h.flags = 0;
    route(0, 5, msi_addr(0, 0), 0);
    rt.e[0].type = KVM_IRQ_ROUTING_IRQCHIP;
    CHECK(kvm_irq_ioctl(&vm, KVM_SET_GSI_ROUTING, (uintptr_t)&rt) == -EINVAL,
          "an irqchip route with the split controller");
    rt.e[0].type = 99;
    CHECK(kvm_irq_ioctl(&vm, KVM_SET_GSI_ROUTING, (uintptr_t)&rt) == -EINVAL,
          "a route type not offered");
    sint_route_rows();
    route(0, 4096, msi_addr(0, 0), 0);
    CHECK(kvm_irq_ioctl(&vm, KVM_SET_GSI_ROUTING, (uintptr_t)&rt) == -EINVAL, "a GSI past 4095");
    rt.h.nr = 4097;
    CHECK(kvm_irq_ioctl(&vm, KVM_SET_GSI_ROUTING, (uintptr_t)&rt) == -EINVAL, "too many routes");
    rt.h.nr = 1;
    rt.h.flags = 1;
    CHECK(kvm_irq_ioctl(&vm, KVM_SET_GSI_ROUTING, (uintptr_t)&rt) == -EINVAL, "a flag");
    rt.h.flags = 0;
    rt.h.nr = 2;
    route(0, 5, msi_addr(0, 0), msi_data(0x35, 0, 1));
    route(1, 5, msi_addr(0, 0), msi_data(0x36, 0, 0));
    CHECK(kvm_irq_ioctl(&vm, KVM_SET_GSI_ROUTING, (uintptr_t)&rt) == -EINVAL,
          "two MSI routes of one GSI");
    invalid_route_rows();
    route(0, 5, msi_addr(0, 0), msi_data(0x35, 0, 1));
    rt.h.nr = 3;
    route(1, 6, msi_addr(0, 0), msi_data(0x36, 0, 0));
    route(2, 30, msi_addr(0, 0), msi_data(0x37, 0, 1));
    CHECK(kvm_irq_ioctl(&vm, KVM_SET_GSI_ROUTING, (uintptr_t)&rt) == 0, "three MSI routes");
    CHECK(kvm_irq_ioctl(&vm, KVM_SET_GSI_ROUTING, 0) == -EFAULT, "a bad address");

    clear_vectors();
    CHECK(scan_eoi_exits(), "the routes ask each vCPU to find its EOI exits again before entry");
    lvl.irq = 5;
    lvl.level = 1;
    CHECK(kvm_irq_ioctl(&vm, KVM_IRQ_LINE_STATUS, (uintptr_t)&lvl) == 0 && lvl.irq == 1,
          "KVM_IRQ_LINE_STATUS reports the APIC that took it");
    CHECK(has(v, APIC_IRR, 0x35) && has(v, APIC_TMR, 0x35), "a level-triggered route");
    lvl.irq = 5;
    lvl.level = 0;
    kvm_irq_ioctl(&vm, KVM_IRQ_LINE_STATUS, (uintptr_t)&lvl);
    CHECK(lvl.irq == -1, "a line lowered on an MSI route is -1");
    lvl.irq = 9;
    lvl.level = 1;
    kvm_irq_ioctl(&vm, KVM_IRQ_LINE_STATUS, (uintptr_t)&lvl);
    CHECK(lvl.irq == -1, "a GSI with no route is -1");
    CHECK(kvm_lapic_ack(v) == 0x35, "acknowledged");
    wr(v, APIC_EOI, 0);
    CHECK(v->requests & KVM_REQ_EOI_EXIT, "the EOI of an I/O APIC level route asks to exit");
    CHECK(kvm_lapic_prepare(v) == 1 && v->run->exit_reason == KVM_EXIT_IOAPIC_EOI &&
              v->run->u.eoi.vector == 0x35,
          "KVM_EXIT_IOAPIC_EOI with the vector");
    lvl.irq = 6;
    kvm_irq_ioctl(&vm, KVM_IRQ_LINE, (uintptr_t)&lvl);
    kvm_lapic_ack(v);
    wr(v, APIC_EOI, 0);
    CHECK(!(v->requests & KVM_REQ_EOI_EXIT), "an edge-triggered route's EOI stays in the kernel");
    lvl.irq = 30;
    kvm_irq_ioctl(&vm, KVM_IRQ_LINE, (uintptr_t)&lvl);
    kvm_lapic_ack(v);
    wr(v, APIC_EOI, 0);
    CHECK(!(v->requests & KVM_REQ_EOI_EXIT), "a level route past the I/O APIC's pins too");
    eoi_exit_rows();
    CHECK(signal(msi_addr(0, 0), msi_data(0x38, 0, 0)) == 1, "KVM_SIGNAL_MSI");
    {
        struct kvm_msi m;

        k_memset(&m, 0, sizeof(m));
        m.flags = 2;
        CHECK(kvm_irq_ioctl(&vm, KVM_SIGNAL_MSI, (uintptr_t)&m) == -EINVAL, "an MSI flag");
        CHECK(kvm_irq_ioctl(&none, KVM_SIGNAL_MSI, (uintptr_t)&m) == -EINVAL,
              "KVM_SIGNAL_MSI with no controller");
    }
    clear_vectors();
}

/** irqfds over the kernel's eventfd. */
static void irqfd_rows(void)
{
    struct kvm_irqfd a;
    struct eventfd *ev;
    struct irqfd *f;
    int before = live;

    rt.h.nr = 1;
    route(0, 40, msi_addr(1, 0), msi_data(0x71, 0, 0));
    kvm_irq_ioctl(&vm, KVM_SET_GSI_ROUTING, (uintptr_t)&rt);
    ev = efd_open(&efd_entry);
    k_memset(&a, 0, sizeof(a));
    a.fd = 3;
    a.gsi = 40;
    CHECK(kvm_irq_ioctl(&vm, KVM_IRQFD, (uintptr_t)&a) == 0 && ev->refs == 2,
          "KVM_IRQFD holds the eventfd");
    clear_vectors();
    CHECK(efd_write(&efd_entry, 1) == 8, "a write to the eventfd");
    CHECK(has(vcpu[1], APIC_IRR, 0x71), "raises the GSI's MSI at the APIC");
    CHECK(ev->count == 0, "and the irqfd takes the counter");
    CHECK(kvm_irq_ioctl(&vm, KVM_IRQFD, (uintptr_t)&a) == -EBUSY, "the eventfd twice");
    a.flags = KVM_IRQFD_FLAG_RESAMPLE;
    CHECK(kvm_irq_ioctl(&vm, KVM_IRQFD, (uintptr_t)&a) == -EINVAL,
          "RESAMPLE needs the I/O APIC in the kernel");
    a.flags = 4;
    CHECK(kvm_irq_ioctl(&vm, KVM_IRQFD, (uintptr_t)&a) == -EINVAL, "an unknown flag");
    a.flags = 0;
    a.fd = 9;
    CHECK(kvm_irq_ioctl(&vm, KVM_IRQFD, (uintptr_t)&a) == -EBADF, "no descriptor");
    a.fd = 3;
    route(0, 40, msi_addr(2, 0), msi_data(0x72, 0, 0));
    kvm_irq_ioctl(&vm, KVM_SET_GSI_ROUTING, (uintptr_t)&rt);
    efd_write(&efd_entry, 1);
    CHECK(has(vcpu[2], APIC_IRR, 0x72), "the irqfd follows a new route");
    a.flags = KVM_IRQFD_FLAG_DEASSIGN;
    CHECK(kvm_irq_ioctl(&vm, KVM_IRQFD, (uintptr_t)&a) == 0 && ev->refs == 1, "deassigned");
    clear_vectors();
    efd_write(&efd_entry, 1);
    CHECK(!has(vcpu[2], APIC_IRR, 0x72) && ev->count == 1, "a write after reaches nothing");
    a.flags = 0;
    kvm_irq_ioctl(&vm, KVM_IRQFD, (uintptr_t)&a);
    CHECK(has(vcpu[2], APIC_IRR, 0x72), "a counter already up raises it at once");
    eventfd_close(&efd_entry);
    CHECK(irq_of(&vm)->irqfds == NULL && live == before, "the last close frees the irqfd");

    ev = efd_open(&efd_entry);
    kvm_irq_ioctl(&vm, KVM_IRQFD, (uintptr_t)&a);
    f = irq_of(&vm)->irqfds;
    /* A close that detached the hooks, its hangup of this one yet to run
     * after another hangup slept. */
    ev->closed = 1;
    ev->hooks = NULL;
    kvm_irq_free(&vm);
    CHECK(f->vm == NULL && ev->refs == 2, "the VM's end leaves a detached irqfd to its hangup");
    {
        struct kvm_enable_cap cap;

        k_memset(&cap, 0, sizeof(cap));
        cap.args[0] = 24;
        vm.irqchip = KVM_IRQCHIP_NONE;
        vm.nr_vcpus = 0;
        kvm_irq_enable_split(&vm, &cap);
        vm.nr_vcpus = NVCPU;
    }
    before = live;
    irqfd_hangup(&f->hook);
    CHECK(ev->refs == 1, "whose hangup lets go of the eventfd");
    eventfd_close(&efd_entry);
    CHECK(live == before - 2, "the hangup frees the detached irqfd");
}

/* The ioeventfd ioeventfd_rows asks for. */
static struct kvm_ioeventfd io;

/** KVM_IOEVENTFD with `io` as set. */
static int32_t ioev(void)
{
    return kvm_iobus_ioctl(&vm, KVM_IOEVENTFD, (uintptr_t)&io);
}

/** A port OUT of `count` elements of `size` bytes of `value` at `port`,
 * offered to the bus: whether it took it. */
static int out(uint16_t port, uint8_t size, uint32_t count, uint32_t value)
{
    struct kvm_vcpu *v = vcpu[0];
    uint32_t i, b;

    k_memset(&v->exit, 0, sizeof(v->exit));
    v->exit.kind = KVM_X_IO;
    v->exit.io.direction = KVM_EXIT_IO_OUT;
    v->exit.io.port = port;
    v->exit.io.size = size;
    v->exit.io.count = count;
    for (i = 0; i < count; i++) {
        for (b = 0; b < size; b++) {
            v->exit.data[i * size + b] = (uint8_t)(value >> (b * 8u));
        }
    }
    return kvm_iobus_write(v);
}

/** An MMIO write of `len` bytes at `addr`, offered to the bus. */
static int mwrite(uint64_t addr, uint32_t len, uint32_t value)
{
    struct kvm_vcpu *v = vcpu[0];

    k_memset(&v->exit, 0, sizeof(v->exit));
    v->exit.kind = KVM_X_MMIO;
    v->exit.mmio.phys_addr = addr;
    v->exit.mmio.len = len;
    v->exit.mmio.is_write = 1;
    k_memcpy(v->exit.mmio.data, &value, 4);
    return kvm_iobus_write(v);
}

/** ioeventfds on both buses. */
static void ioeventfd_rows(void)
{
    struct eventfd *ev = efd_open(&efd2_entry);
    int before = live;
    uint32_t i;

    k_memset(&io, 0, sizeof(io));
    io.fd = 5;
    io.flags = KVM_IOEVENTFD_FLAG_PIO;
    io.addr = 0x510;
    io.len = 3;
    CHECK(ioev() == -EINVAL, "a length not 0, 1, 2, 4 or 8");
    io.len = 0;
    io.flags |= KVM_IOEVENTFD_FLAG_DATAMATCH;
    CHECK(ioev() == -EINVAL, "a data match with no length");
    io.flags = 16;
    io.len = 2;
    CHECK(ioev() == -EINVAL, "an unknown flag");
    io.flags = 0;
    io.addr = ~0ull;
    CHECK(ioev() == -EINVAL, "a range that wraps");
    io.flags = KVM_IOEVENTFD_FLAG_PIO;
    io.addr = 0x510;
    io.fd = 9;
    CHECK(ioev() == -EBADF, "no descriptor");
    io.fd = 5;
    CHECK(ioev() == 0 && ev->refs == 2, "a port of two bytes, any value");
    CHECK(ioev() == -EEXIST, "the same twice");
    CHECK(out(0x510, 2, 1, 0x1234) == 1 && ev->count == 1, "an OUT there signals it");
    CHECK(out(0x510, 1, 1, 0x12) == 0, "an OUT of another length goes to the caller");
    CHECK(out(0x511, 2, 1, 0x12) == 0, "another port goes to the caller");
    CHECK(out(0x510, 2, 3, 0x1234) == 1 && ev->count == 4, "a string OUT signals each element");
    io.addr = 0x520;
    io.len = 4;
    io.flags = KVM_IOEVENTFD_FLAG_PIO | KVM_IOEVENTFD_FLAG_DATAMATCH;
    io.datamatch = 0xABCD;
    CHECK(ioev() == 0, "a port with a data match");
    io.datamatch = 0x1234;
    CHECK(ioev() == 0, "another value at the same port");
    CHECK(out(0x520, 4, 1, 0xABCD) == 1 && ev->count == 5, "the matching value signals");
    CHECK(out(0x520, 4, 1, 0x9999) == 0, "another value goes to the caller");
    io.flags = KVM_IOEVENTFD_FLAG_PIO;
    CHECK(ioev() == -EEXIST, "any value collides with a data match");
    io.len = 0;
    CHECK(ioev() == -EEXIST, "no length collides with any length");
    io.addr = 0xFEB00000u;
    io.flags = 0;
    CHECK(ioev() == 0, "an MMIO address of no length");
    CHECK(mwrite(0xFEB00000u, 4, 7) == 1 && mwrite(0xFEB00000u, 1, 7) == 1 && ev->count == 7,
          "a write of any length there signals it");
    CHECK(out(0xFEB0, 4, 1, 7) == 0 && mwrite(0xFEB00004u, 4, 7) == 0,
          "the port bus and another address do not");
    io.flags = KVM_IOEVENTFD_FLAG_DEASSIGN;
    io.len = 4;
    CHECK(ioev() == -ENOENT, "deassign of another length");
    io.len = 0;
    CHECK(ioev() == 0 && mwrite(0xFEB00000u, 4, 7) == 0, "deassigned");
    io.addr = 0x520;
    io.len = 4;
    io.flags = KVM_IOEVENTFD_FLAG_PIO | KVM_IOEVENTFD_FLAG_DEASSIGN | KVM_IOEVENTFD_FLAG_DATAMATCH;
    io.datamatch = 0x5555;
    CHECK(ioev() == -ENOENT, "deassign of another value");
    io.datamatch = 0xABCD;
    CHECK(ioev() == 0 && out(0x520, 4, 1, 0xABCD) == 0, "the matching one deassigned");
    io.flags = KVM_IOEVENTFD_FLAG_PIO;
    io.len = 1;
    for (i = 0; i <= MAX_BUS_DEVS; i++) {
        io.addr = 0x1000 + i;
        if (ioev() != 0) {
            break;
        }
    }
    CHECK(i == MAX_BUS_DEVS + 1, "ioeventfds count toward no bus's 1000, as on Linux");
    io.flags = 0;
    CHECK(ioev() == 0, "and take the MMIO bus too");
    kvm_iobus_free(&vm);
    CHECK(ev->refs == 1 && live == before, "the VM's end lets go of every ioeventfd");
    eventfd_close(&efd2_entry);
}

/* The ring coalesced_rows gives the VM, and the zone it asks for. */
static struct kvm_coalesced_mmio_ring ring;
static struct kvm_coalesced_mmio_zone zone;

/** KVM_REGISTER_COALESCED_MMIO or its undoing of `size` bytes at `addr`. */
static int32_t coalesced(uint32_t request, uint64_t addr, uint32_t size, uint32_t pio)
{
    zone.addr = addr;
    zone.size = size;
    zone.pio = pio;
    return kvm_iobus_ioctl(&vm, request, (uintptr_t)&zone);
}

/** Fill the ring from slot `start` as coalesced_io_test does, an 8-byte
 * MMIO write and a 4-byte OUT by turns: whether every write but the one
 * that would take the last free slot was queued whole, and that one went
 * to the caller. */
static int ring_fills(uint32_t start)
{
    const uint64_t gpa = 0x100000000ull;
    uint32_t i, at;
    int ok = 1;

    ring.first = ring.last = start;
    for (i = 0; i < KVM_COALESCED_MMIO_MAX - 1u; i++) {
        ok = ok && ((i & 1u) ? out(0x80, 4, 1, 0x80 + i) : mwrite(gpa, 8, i)) == 1;
    }
    ok = ok && (ring.last + 1u) % KVM_COALESCED_MMIO_MAX == ring.first && ring.first == start;
    ok = ok && mwrite(gpa, 8, i) == 0 && out(0x80, 4, 1, 0x80 + i) == 0;
    for (i = 0; i < KVM_COALESCED_MMIO_MAX - 1u; i++) {
        const struct kvm_coalesced_mmio *e;
        uint32_t value;

        at = (start + i) % KVM_COALESCED_MMIO_MAX;
        e = &ring.coalesced_mmio[at];
        k_memcpy(&value, e->data, 4);
        if (i & 1u) {
            ok = ok && e->phys_addr == 0x80 && e->len == 4 && e->pio == 1 && value == 0x80 + i;
        } else {
            ok = ok && e->phys_addr == gpa && e->len == 8 && e->pio == 0 && value == i;
        }
    }
    return ok;
}

/** What a zone takes and what removes it, with the zones of
 * coalesced_rows in place. */
static void range_rows(void)
{
    const uint64_t gpa = 0x100000000ull;

    ring.first = 0;
    ring.last = KVM_COALESCED_MMIO_MAX;
    CHECK(mwrite(gpa, 8, 1) == 0 && ring.last == KVM_COALESCED_MMIO_MAX,
          "a last the caller put outside the ring queues nothing");
    ring.first = ring.last = 0;
    CHECK(mwrite(gpa + 4, 4, 1) == 1 && mwrite(gpa + 4, 8, 1) == 0 && mwrite(gpa - 1, 2, 1) == 0 &&
              ring.last == 1,
          "a write must lie wholly in its zone");
    CHECK(out(0x84, 4, 1, 1) == 1 && out(0x86, 4, 1, 1) == 0 && out(0x88, 1, 1, 1) == 0 &&
              ring.last == 2 && ring.coalesced_mmio[1].phys_addr == 0x84,
          "a port write too");
    CHECK(mwrite(0x80, 4, 1) == 0 && out(0x80, 2, 3, 7) == 1 && ring.last == 5 &&
              ring.coalesced_mmio[4].len == 2 && ring.coalesced_mmio[4].pio == 1,
          "the buses are apart, and a string OUT queues each element");
    CHECK(coalesced(KVM_UNREGISTER_COALESCED_MMIO, gpa, 16, 0) == 0 && mwrite(gpa, 8, 1) == 1,
          "a range the zone does not hold removes nothing");
    CHECK(coalesced(KVM_REGISTER_COALESCED_MMIO, gpa, 16, 0) == 0 &&
              coalesced(KVM_UNREGISTER_COALESCED_MMIO, gpa + 4, 4, 0) == 0 &&
              mwrite(gpa, 8, 1) == 0,
          "a range removes every zone that holds it");
    CHECK(out(0x80, 4, 1, 1) == 1 && coalesced(KVM_UNREGISTER_COALESCED_MMIO, 0x80, 8, 1) == 0 &&
              out(0x80, 4, 1, 1) == 0,
          "the port zone goes by its own bus");
}

/** Coalesced zones on both buses. */
static void coalesced_rows(void)
{
    const uint64_t gpa = 0x100000000ull;
    long before = live;
    uint32_t i;

    vm.ring = &ring;
    CHECK(sizeof(struct kvm_coalesced_mmio_zone) == 16 && sizeof(struct kvm_coalesced_mmio) == 24 &&
              sizeof(ring) <= 4096 && KVM_COALESCED_MMIO_MAX == 170,
          "the zone, the entry and the ring of 170 are Linux's");
    CHECK(kvm_iobus_check_extension(KVM_CAP_COALESCED_MMIO) == KVM_COALESCED_MMIO_PAGE_OFFSET &&
              kvm_iobus_check_extension(KVM_CAP_COALESCED_PIO) == 1,
          "KVM_CAP_COALESCED_MMIO is the ring's page and KVM_CAP_COALESCED_PIO 1");
    CHECK(coalesced(KVM_REGISTER_COALESCED_MMIO, gpa, 8, 2) == -EINVAL &&
              coalesced(KVM_UNREGISTER_COALESCED_MMIO, gpa, 8, 2) == -EINVAL,
          "a pio that is not 0 or 1 is -EINVAL");
    CHECK(kvm_iobus_ioctl(&vm, KVM_REGISTER_COALESCED_MMIO, 0) == -EFAULT, "a bad zone is -EFAULT");
    CHECK(coalesced(KVM_UNREGISTER_COALESCED_MMIO, gpa, 8, 0) == 0, "removing no zone is no error");
    CHECK(mwrite(gpa, 8, 1) == 0 && out(0x80, 4, 1, 1) == 0,
          "a write in no zone goes to the caller");
    CHECK(coalesced(KVM_REGISTER_COALESCED_MMIO, gpa, 8, 0) == 0 &&
              coalesced(KVM_REGISTER_COALESCED_MMIO, 0x80, 8, 1) == 0,
          "a zone of MMIO and a zone of ports");
    CHECK(ring_fills(0), "the zones queue all but the last slot's write");
    CHECK(ring_fills(1) && ring_fills(85) && ring_fills(KVM_COALESCED_MMIO_MAX - 1u),
          "from any start, around the ring's end");
    range_rows();
    for (i = 0; i < MAX_BUS_DEVS; i++) {
        if (coalesced(KVM_REGISTER_COALESCED_MMIO, 0x1000 + i, 1, 1) != 0) {
            break;
        }
    }
    CHECK(i == MAX_BUS_DEVS && coalesced(KVM_REGISTER_COALESCED_MMIO, 0x9000, 1, 1) == -ENOSPC,
          "the port bus holds 1000 zones and no more");
    efd_open(&efd2_entry);
    k_memset(&io, 0, sizeof(io));
    io.fd = 5;
    io.flags = KVM_IOEVENTFD_FLAG_PIO;
    io.addr = 0x9100;
    io.len = 1;
    CHECK(ioev() == -ENOSPC, "nor an ioeventfd beside them");
    io.flags = 0;
    CHECK(ioev() == 0, "while the MMIO bus takes one");
    kvm_iobus_free(&vm);
    eventfd_close(&efd2_entry);
    CHECK(live == before, "the VM's end frees every zone");
    vm.ring = NULL;
}

/** A byte access through the vCPU's legacy port exit. */
static uint32_t legacy_port(uint32_t port, int write, uint32_t value)
{
    struct kvm_vcpu *v = vcpu[0];

    k_memset(&v->exit, 0, sizeof(v->exit));
    v->exit.kind = KVM_X_IO;
    v->exit.io.port = (uint16_t)port;
    v->exit.io.size = 1;
    v->exit.io.count = 1;
    v->exit.io.direction = write ? KVM_EXIT_IO_OUT : KVM_EXIT_IO_IN;
    v->exit.data[0] = (uint8_t)value;
    CHECK(kvm_irq_io(v) == 1, "the legacy controller takes its port");
    return v->exit.data[0];
}

/** Write an IOAPIC register through IOREGSEL and IOWIN. */
static void ioa_write(uint32_t reg, uint32_t value)
{
    struct kvm_vcpu *v = vcpu[0];
    uint32_t i, words[2];

    words[0] = reg;
    words[1] = value;
    for (i = 0; i < 2; i++) {
        k_memset(&v->exit, 0, sizeof(v->exit));
        v->exit.kind = KVM_X_MMIO;
        v->exit.mmio.phys_addr = 0xFEC00000ull + i * 16u;
        v->exit.mmio.len = 4;
        v->exit.mmio.is_write = 1;
        k_memcpy(v->exit.mmio.data, &words[i], 4);
        CHECK(kvm_irq_io(v) == 1, "the IOAPIC takes its MMIO");
    }
}

/** Program one IOAPIC entry's vector, flags and physical destination. */
static void ioa_entry(uint32_t pin, uint32_t low, uint32_t destination)
{
    ioa_write(0x11u + pin * 2u, destination << 24);
    ioa_write(0x10u + pin * 2u, low);
}

/** Set a userspace GSI and return Linux's delivery status. */
static int line_status(uint32_t gsi, uint32_t level)
{
    struct kvm_irq_level line;

    line.irq = (int32_t)gsi;
    line.level = level;
    CHECK(kvm_irq_ioctl(&vm, KVM_IRQ_LINE_STATUS, (uintptr_t)&line) == 0,
          "IRQ_LINE_STATUS accepts a line");
    return line.irq;
}

/** Initialize both PICs to vectors 0x20 and 0x28, all inputs unmasked. */
static void pic_program(int automatic)
{
    uint32_t chip, port;

    for (chip = 0; chip < 2; chip++) {
        port = chip ? 0xA0u : 0x20u;
        legacy_port(port, 1, 0x11);
        legacy_port(port + 1, 1, chip ? 0x28u : 0x20u);
        legacy_port(port + 1, 1, chip ? 2 : 4);
        legacy_port(port + 1, 1, automatic ? 3 : 1);
        legacy_port(port + 1, 1, 0);
    }
}

/** The PIC's vectors, cascade, priority, levels, polling and state ABI. */
static void pic_rows(void)
{
    struct kvm_irqchip chip;
    struct kvm_interrupt intr;
    struct kvm_vcpu *v = vcpu[0];

    pic_program(0);
    CHECK(line_status(1, 1) == 1 && kvm_lapic_pending(v) == 0x21,
          "the default master PIC route reaches LINT0");
    line_status(1, 0);
    CHECK(kvm_lapic_ack(v) == 0x21 && !has(v, APIC_ISR, 0x21),
          "PIC INTA does not put its vector in the local APIC ISR");
    line_status(3, 1);
    line_status(3, 0);
    CHECK(kvm_lapic_pending(v) == -1, "a lower PIC priority waits for EOI");
    legacy_port(0x20, 1, 0x0B);
    CHECK(legacy_port(0x20, 0, 0) == 2, "OCW3 selects the PIC ISR");
    legacy_port(0x20, 1, 0x20);
    CHECK(kvm_lapic_ack(v) == 0x23, "EOI releases the next PIC interrupt");
    legacy_port(0x20, 1, 0x20);
    line_status(10, 1);
    line_status(10, 0);
    CHECK(kvm_lapic_ack(v) == 0x2A, "the slave is acknowledged through IRQ2");
    legacy_port(0xA0, 1, 0x0B);
    CHECK(legacy_port(0x20, 0, 0) == 4 && legacy_port(0xA0, 0, 0) == 4,
          "both cascade pins are in service");
    legacy_port(0xA0, 1, 0x20);
    legacy_port(0x20, 1, 0x20);
    legacy_port(0x4D0, 1, 0xFF);
    CHECK(legacy_port(0x4D0, 0, 0) == 0xF8, "ELCR keeps its read-only pins clear");
    line_status(5, 1);
    CHECK(kvm_lapic_ack(v) == 0x25, "a level input arrives");
    legacy_port(0x20, 1, 0x20);
    CHECK(kvm_lapic_ack(v) == 0x25, "an asserted level arrives again after EOI");
    line_status(5, 0);
    legacy_port(0x20, 1, 0x20);
    CHECK(kvm_lapic_pending(v) == -1, "a lowered level stays down after EOI");
    legacy_port(0x4D0, 1, 0);
    line_status(4, 1);
    line_status(4, 0);
    legacy_port(0x20, 1, 0x0C);
    CHECK(legacy_port(0x20, 0, 0) == 0x84 && kvm_lapic_pending(v) == -1,
          "OCW3 poll acknowledges its pin");
    pic_program(1);
    line_status(1, 1);
    line_status(1, 0);
    CHECK(kvm_lapic_ack(v) == 0x21 && legacy_port(0x20, 0, 0) == 0,
          "automatic EOI ends service at INTA");
    k_memset(&chip, 0xA5, sizeof(chip));
    chip.chip_id = KVM_IRQCHIP_PIC_MASTER;
    CHECK(kvm_irq_ioctl(&vm, KVM_GET_IRQCHIP, (uintptr_t)&chip) == 0 &&
              chip.chip.pic.irq_base == 0x20 && chip.chip.dummy[511] == 0xA5,
          "GET_IRQCHIP preserves unused bytes");
    chip.chip.pic.irq_base = 0x30;
    chip.chip.pic.irr = 2;
    CHECK(kvm_irq_ioctl(&vm, KVM_SET_IRQCHIP, (uintptr_t)&chip) == 0 && kvm_lapic_ack(v) == 0x31,
          "restoring a PIC updates its pending vector");
    intr.irq = 0x40;
    CHECK(kvm_lapic_ioctl(v, KVM_INTERRUPT, (uintptr_t)&intr) == -ENXIO,
          "KVM_INTERRUPT is refused with the full controller");
    legacy_port(0x21, 1, 0xFF);
    legacy_port(0xA1, 1, 0xFF);
}

/** IOAPIC edges, level EOI, read-only bits and RTC saved state. */
static void ioapic_rows(void)
{
    struct kvm_irqchip chip;
    struct kvm_lapic_state local;
    struct kvm_vcpu *v = vcpu[0];
    uint32_t remote;

    wr(v, APIC_SPIV, 0x1FF);
    wr(v, APIC_LVT0, APIC_LVT_MASKED);
    ioa_entry(18, 0x51, 0);
    CHECK(line_status(18, 1) == 1 && kvm_lapic_ack(v) == 0x51,
          "the default IOAPIC route reaches the local APIC");
    CHECK(line_status(18, 1) == 0, "an unchanged edge input coalesces");
    line_status(18, 0);
    wr(v, APIC_EOI, 0);
    ioa_entry(19, 0x52u | (uint32_t)IOA_LEVEL, 0);
    CHECK(line_status(19, 1) == 1 && kvm_lapic_ack(v) == 0x52, "a level input is accepted");
    remote = (uint32_t)IOA_REMOTE;
    ioa_write(0x10u + 19u * 2u, 0x52u | (uint32_t)IOA_LEVEL);
    k_memset(&chip, 0, sizeof(chip));
    chip.chip_id = KVM_IRQCHIP_IOAPIC;
    kvm_irq_ioctl(&vm, KVM_GET_IRQCHIP, (uintptr_t)&chip);
    CHECK(chip.chip.ioapic.redirtbl[19] & remote, "a guest write cannot clear remote IRR");
    wr(v, APIC_EOI, 0);
    CHECK(kvm_lapic_ack(v) == 0x52 && !(v->requests & KVM_REQ_EOI_EXIT),
          "the level EOI reinjects inside KVM");
    line_status(19, 0);
    wr(v, APIC_EOI, 0);
    CHECK(kvm_lapic_pending(v) == -1, "the low input does not reinject");
    kvm_irq_ioctl(&vm, KVM_GET_IRQCHIP, (uintptr_t)&chip);
    CHECK(!(chip.chip.ioapic.redirtbl[19] & remote), "EOI clears remote IRR");
    ioa_entry(20, 0x53u | (uint32_t)(IOA_LEVEL | IOA_MASKED), 0);
    CHECK(line_status(20, 1) == -1, "a masked level stays pending");
    ioa_entry(20, 0x53u | (uint32_t)IOA_LEVEL, 0);
    CHECK(kvm_lapic_ack(v) == 0x53, "unmask delivers the pending level");
    line_status(20, 0);
    wr(v, APIC_EOI, 0);
    ioa_entry(8, 0x58, 0);
    CHECK(line_status(8, 1) == 1, "an RTC edge is delivered");
    line_status(8, 0);
    CHECK(line_status(8, 1) == 0, "an RTC edge waits for the previous EOI");
    line_status(8, 0);
    CHECK(kvm_lapic_ack(v) == 0x58, "the RTC vector is acknowledged");
    kvm_lapic_ioctl(v, KVM_GET_LAPIC, (uintptr_t)&local);
    kvm_irq_ioctl(&vm, KVM_GET_IRQCHIP, (uintptr_t)&chip);
    kvm_irq_ioctl(&vm, KVM_SET_IRQCHIP, (uintptr_t)&chip);
    kvm_lapic_ioctl(v, KVM_SET_LAPIC, (uintptr_t)&local);
    CHECK(line_status(8, 1) == 0, "restoring both APICs retains the RTC acknowledgement");
    line_status(8, 0);
    wr(v, APIC_EOI, 0);
    CHECK(line_status(8, 1) == 1, "the RTC EOI permits another edge");
    line_status(8, 0);
    kvm_lapic_ack(v);
    wr(v, APIC_EOI, 0);
    chip.chip_id = 3;
    CHECK(kvm_irq_ioctl(&vm, KVM_GET_IRQCHIP, (uintptr_t)&chip) == -EINVAL,
          "an unknown controller id is refused");
    CHECK(kvm_irq_ioctl(&vm, KVM_GET_IRQCHIP, 0) == -EFAULT,
          "an invalid controller-state address is refused");
}

/** Level irqfds defer interrupt-context work and resample after EOI. */
static void resample_rows(void)
{
    struct kvm_irqfd a;
    struct eventfd *ev = efd_open(&efd_entry), *resample = efd_open(&efd2_entry);
    struct kvm_vcpu *v = vcpu[0];

    ioa_entry(21, 0x61u | (uint32_t)IOA_LEVEL, 0);
    k_memset(&a, 0, sizeof(a));
    a.fd = 3;
    a.resamplefd = 5;
    a.gsi = 21;
    a.flags = KVM_IRQFD_FLAG_RESAMPLE;
    CHECK(kvm_irq_ioctl(&vm, KVM_IRQFD, (uintptr_t)&a) == 0 && resample->refs == 2,
          "a resampling irqfd holds both eventfds");
    efd_write(&efd_entry, 1);
    CHECK(ev->count == 0 && kvm_irq_pending(&vm) && !has(v, APIC_IRR, 0x61),
          "its interrupt-context hook consumes the counter and defers the line");
    kvm_irq_process(&vm);
    CHECK(kvm_lapic_ack(v) == 0x61 && !kvm_irq_pending(&vm),
          "the deferred irqfd raises its line before guest entry");
    wr(v, APIC_EOI, 0);
    CHECK(resample->count == 1 && kvm_lapic_pending(v) == -1,
          "EOI lowers the resampling source and signals the resamplefd");
    line_status(21, 1);
    kvm_lapic_ack(v);
    efd_write(&efd_entry, 1);
    kvm_irq_process(&vm);
    wr(v, APIC_EOI, 0);
    CHECK(kvm_lapic_ack(v) == 0x61,
          "resampling preserves another source that still holds the line high");
    line_status(21, 0);
    wr(v, APIC_EOI, 0);
    a.flags = KVM_IRQFD_FLAG_DEASSIGN;
    CHECK(kvm_irq_ioctl(&vm, KVM_IRQFD, (uintptr_t)&a) == 0 && resample->refs == 1,
          "deassign releases the resample eventfd");
    eventfd_close(&efd_entry);
    eventfd_close(&efd2_entry);
}

/** The PIT's binary count, latches, pending ticks, speaker and HPET handoff. */
static void pit_rows(void)
{
    struct kvm_pit_config config;
    struct kvm_pit_state2 state;
    struct kvm_reinject_control control;
    struct kvm_vcpu *v = vcpu[0];
    uint32_t lo, hi, status;
    uint64_t saved;

    k_memset(&config, 0, sizeof(config));
    config.flags = KVM_PIT_SPEAKER_DUMMY;
    CHECK(kvm_irq_ioctl(&vm, KVM_CREATE_PIT2, (uintptr_t)&config) == 0,
          "CREATE_PIT2 makes the timer and silent speaker");
    CHECK(kvm_irq_ioctl(&vm, KVM_CREATE_PIT, 0) == -EEXIST, "the PIT is made once");
    CHECK(kvm_irq_ioctl(&vm, KVM_GET_PIT2, (uintptr_t)&state) == 0 &&
              state.channels[0].count == 65536 && state.channels[2].gate == 0,
          "the PIT's reset counts and speaker gate");
    legacy_port(0x43, 1, 0xB6);
    legacy_port(0x42, 1, 0x34);
    legacy_port(0x42, 1, 0x12);
    legacy_port(0x61, 1, 3);
    CHECK((legacy_port(0x61, 0, 0) & 3) == 3, "speaker data and gate are retained");
    legacy_port(0x43, 1, 0xC8);
    status = legacy_port(0x42, 0, 0);
    lo = legacy_port(0x42, 0, 0);
    mono_ns += 1000000;
    hi = legacy_port(0x42, 0, 0);
    CHECK((status & 0x3F) == 0x36 && (lo | (hi << 8)) == 0x1234,
          "read-back returns status first and keeps both count bytes latched");
    ioa_entry(0, 0x70, 0);
    legacy_port(0x43, 1, 0x34);
    legacy_port(0x40, 1, 0xA9);
    legacy_port(0x40, 1, 0x04);
    saved = mono_ns;
    mono_ns += 3100000;
    kvm_pit_check(&vm);
    CHECK(kvm_lapic_ack(v) == 0x70, "three elapsed PIT periods deliver the first tick");
    kvm_pit_check(&vm);
    CHECK(!has(v, APIC_IRR, 0x70), "reinjection waits for the first EOI");
    wr(v, APIC_EOI, 0);
    kvm_pit_check(&vm);
    CHECK(kvm_lapic_ack(v) == 0x70, "the first EOI releases the second tick");
    wr(v, APIC_EOI, 0);
    kvm_pit_check(&vm);
    CHECK(kvm_lapic_ack(v) == 0x70, "the second EOI releases the third tick");
    wr(v, APIC_EOI, 0);
    kvm_pit_check(&vm);
    CHECK(!has(v, APIC_IRR, 0x70), "the backlog is exhausted without another period");
    k_memset(&control, 0, sizeof(control));
    CHECK(kvm_irq_ioctl(&vm, KVM_REINJECT_CONTROL, (uintptr_t)&control) == 0,
          "reinjection can be disabled");
    mono_ns = saved + 10100000;
    kvm_pit_check(&vm);
    CHECK(kvm_lapic_ack(v) == 0x70, "without reinjection missed periods coalesce");
    wr(v, APIC_EOI, 0);
    kvm_pit_check(&vm);
    CHECK(!has(v, APIC_IRR, 0x70), "without reinjection EOI does not release a backlog");
    control.pit_reinject = 1;
    kvm_irq_ioctl(&vm, KVM_REINJECT_CONTROL, (uintptr_t)&control);
    kvm_irq_ioctl(&vm, KVM_GET_PIT2, (uintptr_t)&state);
    state.flags |= KVM_PIT_FLAGS_HPET_LEGACY;
    kvm_irq_ioctl(&vm, KVM_SET_PIT2, (uintptr_t)&state);
    CHECK(kvm_pit_next(&vm) == 0, "HPET legacy mode stops IRQ0");
    state.flags &= ~KVM_PIT_FLAGS_HPET_LEGACY;
    state.channels[0].mode = 0;
    state.channels[0].count = 1193;
    kvm_irq_ioctl(&vm, KVM_SET_PIT2, (uintptr_t)&state);
    v->regs.rflags = 0x202;
    v->dirty |= KVM_PART_REGS;
    v->mp_state = KVM_MP_STATE_HALTED;
    sleep_to_tick = 1;
    CHECK(kvm_lapic_prepare(v) == 0 && v->mp_state == KVM_MP_STATE_RUNNABLE &&
              kvm_lapic_ack(v) == 0x70,
          "the PIT's one-shot wakes a halted vCPU at its next deadline");
    sleep_to_tick = 0;
    wr(v, APIC_EOI, 0);
    CHECK(kvm_pit_next(&vm) == 0, "the one-shot stays disarmed after EOI");
    k_memset(&state, 0xA5, sizeof(state));
    kvm_irq_ioctl(&vm, KVM_GET_PIT2, (uintptr_t)&state);
    CHECK(state.reserved[0] == 0 && state.reserved[8] == 0, "GET_PIT2 zeros its reserved words");
}

/** Controller routes may share a GSI but never the same chip. */
static void controller_route_rows(void)
{
    uint32_t i;

    k_memset(&rt, 0, sizeof(rt));
    rt.h.nr = 2;
    for (i = 0; i < 2; i++) {
        rt.e[i].gsi = 100;
        rt.e[i].type = KVM_IRQ_ROUTING_IRQCHIP;
    }
    rt.e[0].u.irqchip.irqchip = KVM_IRQCHIP_PIC_SLAVE;
    rt.e[0].u.irqchip.pin = 2;
    rt.e[1].u.irqchip.irqchip = KVM_IRQCHIP_IOAPIC;
    rt.e[1].u.irqchip.pin = 18;
    CHECK(kvm_irq_ioctl(&vm, KVM_SET_GSI_ROUTING, (uintptr_t)&rt) == 0,
          "two different controller routes may share a GSI");
    CHECK(line_status(100, 1) == 1 && kvm_lapic_ack(vcpu[0]) == 0x51,
          "the replacement table routes its IOAPIC pin");
    line_status(100, 0);
    wr(vcpu[0], APIC_EOI, 0);
    CHECK(line_status(18, 1) == -1, "replacement removes the default routes");
    rt.e[0] = rt.e[1];
    CHECK(kvm_irq_ioctl(&vm, KVM_SET_GSI_ROUTING, (uintptr_t)&rt) == -EINVAL,
          "a second route to the same chip is refused");
    rt.h.nr = 1;
    rt.e[0].u.irqchip.pin = KVM_IOAPIC_NUM_PINS;
    CHECK(kvm_irq_ioctl(&vm, KVM_SET_GSI_ROUTING, (uintptr_t)&rt) == -EINVAL,
          "an IOAPIC pin past the table is refused");
    CHECK(line_status(100, 1) == 1 && kvm_lapic_ack(vcpu[0]) == 0x51,
          "a refused table preserves the old routes");
    line_status(100, 0);
    wr(vcpu[0], APIC_EOI, 0);
    rt.h.nr = 0;
    CHECK(kvm_irq_ioctl(&vm, KVM_SET_GSI_ROUTING, (uintptr_t)&rt) == 0 && line_status(100, 1) == -1,
          "an empty table removes every route");
}

/** New full-controller rows, also selectable alone during development. */
static void full_irqchip_rows(void)
{
    struct kvm_vm empty;
    struct kvm_enable_cap cap;
    struct kvm_pit_config pit;
    struct kvm_irqchip chip;
    int i, before = live;

    CHECK(sizeof(struct kvm_irqchip) == 520 && sizeof(struct kvm_ioapic_state) == 216 &&
              sizeof(struct kvm_pit_channel_state) == 24 && sizeof(struct kvm_pit_state2) == 112,
          "the controller records have Linux's ABI sizes");
    CHECK(kvm_irq_check_extension(KVM_CAP_PIT) == 1 && kvm_irq_check_extension(KVM_CAP_PIT2) == 1 &&
              kvm_irq_check_extension(KVM_CAP_PIT_STATE2) == 1 &&
              kvm_irq_check_extension(KVM_CAP_IRQFD_RESAMPLE) == 1 &&
              kvm_irq_check_extension(KVM_CAP_REINJECT_CONTROL) == 1,
          "the controller capabilities are offered");
    k_memset(&empty, 0, sizeof(empty));
    for (i = 0; i < 4; i++) {
        allocation_fail = i;
        CHECK(kvm_irq_ioctl(&empty, KVM_CREATE_IRQCHIP, 0) == -ENOMEM &&
                  empty.irqchip == KVM_IRQCHIP_NONE && !empty.pic && !empty.ioapic && !empty.irq &&
                  live == before,
              "a failed controller allocation leaves no partial state");
    }
    allocation_fail = -1;
    k_memset(&pit, 0, sizeof(pit));
    k_memset(&chip, 0, sizeof(chip));
    CHECK(kvm_irq_ioctl(&empty, KVM_CREATE_PIT2, (uintptr_t)&pit) == -ENOENT,
          "PIT creation needs the full controller");
    CHECK(kvm_irq_ioctl(&empty, KVM_GET_IRQCHIP, (uintptr_t)&chip) == -ENXIO,
          "state ioctls need the full controller");
    empty.nr_vcpus = 1;
    CHECK(kvm_irq_ioctl(&empty, KVM_CREATE_IRQCHIP, 0) == -EINVAL,
          "the full controller must precede the vCPUs");
    vm_make(1);
    k_memset(&cap, 0, sizeof(cap));
    CHECK(kvm_irq_ioctl(&vm, KVM_CREATE_IRQCHIP, 0) == -EEXIST &&
              kvm_irq_enable_split(&vm, &cap) == -EEXIST,
          "a controller cannot be replaced after creation");
    pic_rows();
    ioapic_rows();
    resample_rows();
    pit_rows();
    controller_route_rows();
    vm_end();
}

int main(int argc, char **argv)
{
    int start = live;

    fake.name = "fake";
    fake.complete = fake_complete;
    kvm_backend = &fake;
    if (argc != 1 && (argc != 2 || strcmp(argv[1], "--irqchip-only") != 0)) {
        return 2;
    }
    if (argc == 1) {
        vm_make(0);
        reset_rows();
        priority_rows();
        tpr_shadow_rows();
        delivery_rows();
        init_rows();
        timer_rows();
        x2apic_rows();
        state_rows();
        x2apic_api_rows();
        extint_rows();
        halt_rows();
        routing_rows();
        irqfd_rows();
        ioeventfd_rows();
        coalesced_rows();
        quirk_rows();
        vm_end();
    }
    full_irqchip_rows();
    CHECK(live == start, "every allocation is given back");
    if (failures) {
        printf("kvm-irq-hosttest: %d failures\n", failures);
        return 1;
    }
    printf("kvm-irq-hosttest: ok\n");
    return 0;
}
