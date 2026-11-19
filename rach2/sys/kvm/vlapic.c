/*
 * vlapic.c: the guest's local APIC in the kernel, as Linux's lapic.c keeps
 * it with the split interrupt controller: the register page through MMIO
 * and x2APIC MSRs, the timer in its three modes, the delivery of MSIs and
 * IPIs, INIT and SIPI, the halted vCPU's wait, and KVM_GET_LAPIC and
 * KVM_SET_LAPIC.
 */

#include "kvm.h"

#include "../arch/arch.h"
#include "../lib/kerrno.h"
#include "../lib/kstring.h"
#include "../mm/kheap.h"
#include "../mm/usercopy.h"
#include "../proc/process.h"
#include "../proc/wait_queue.h"
#include "../timer.h"
#include "../rtc.h"

/* The registers, by their offsets in the page. */
#define APIC_ID       0x020u
#define APIC_LVR      0x030u
#define APIC_TASKPRI  0x080u
#define APIC_ARBPRI   0x090u
#define APIC_PROCPRI  0x0A0u
#define APIC_EOI      0x0B0u
#define APIC_LDR      0x0D0u
#define APIC_DFR      0x0E0u
#define APIC_SPIV     0x0F0u
#define APIC_ISR      0x100u
#define APIC_TMR      0x180u
#define APIC_IRR      0x200u
#define APIC_ESR      0x280u
#define APIC_ICR      0x300u
#define APIC_ICR2     0x310u
#define APIC_LVTT     0x320u
#define APIC_LVTTHMR  0x330u
#define APIC_LVTPC    0x340u
#define APIC_LVT0     0x350u
#define APIC_LVT1     0x360u
#define APIC_LVTERR   0x370u
#define APIC_TMICT    0x380u
#define APIC_TMCCT    0x390u
#define APIC_TDCR     0x3E0u
#define APIC_SELF_IPI 0x3F0u

/* The bit of a register in a mask of the page's 64 registers. */
#define REG_BIT(off) (1ull << ((off) >> 4))
#define REGS8(off)   (0xFFull << ((off) >> 4))

/* The registers a read may name, as Linux's kvm_lapic_readable_reg_mask,
 * with no CMCI vector, since no guest is offered CMCI; ARBPRI, DFR and
 * ICR2 only in xAPIC mode. */
#define READABLE                                                                             \
    (REG_BIT(APIC_ID) | REG_BIT(APIC_LVR) | REG_BIT(APIC_TASKPRI) | REG_BIT(APIC_PROCPRI) |  \
     REG_BIT(APIC_LDR) | REG_BIT(APIC_SPIV) | REGS8(APIC_ISR) | REGS8(APIC_TMR) |            \
     REGS8(APIC_IRR) | REG_BIT(APIC_ESR) | REG_BIT(APIC_ICR) | REG_BIT(APIC_LVTT) |          \
     REG_BIT(APIC_LVTTHMR) | REG_BIT(APIC_LVTPC) | REG_BIT(APIC_LVT0) | REG_BIT(APIC_LVT1) | \
     REG_BIT(APIC_LVTERR) | REG_BIT(APIC_TMICT) | REG_BIT(APIC_TMCCT) | REG_BIT(APIC_TDCR))
#define READABLE_XAPIC (REG_BIT(APIC_ARBPRI) | REG_BIT(APIC_DFR) | REG_BIT(APIC_ICR2))

/* The version register: version 0x14 with six LVT entries, and directed
 * EOI where the guest has x2APIC, as Linux's kvm_apic_set_version gives it
 * when the I/O APIC is the caller's. */
#define APIC_VERSION      0x50014u
#define APIC_LVR_DIRECTED (1u << 24)
#define APIC_NR_LVT       6

/* The bits of the registers. */
#define APIC_SPIV_ENABLED  0x100u
#define APIC_SPIV_DIRECTED 0x1000u
#define APIC_LVT_MASKED    0x10000u
#define APIC_SEND_PENDING  0x1000u
#define APIC_VECTOR_MASK   0xFFu
#define APIC_MODE_MASK     0x700u
#define APIC_INPUT_POL     0x2000u
#define APIC_REMOTE_IRR    0x4000u
#define APIC_LEVEL_TRIG    0x8000u
#define APIC_DEST_LOGICAL  0x800u
#define APIC_INT_ASSERT    0x4000u
#define APIC_SHORT_SHIFT   18
#define APIC_SHORT_MASK    0xC0000u
#define APIC_LDR_MASK      0xFF000000u
#define APIC_DFR_FLAT      0xFFFFFFFFu
#define APIC_DFR_CLUSTER   0x0FFFFFFFu
#define APIC_BROADCAST     0xFFu
#define X2APIC_BROADCAST   0xFFFFFFFFu

/* What a write may set in each LVT entry, as Linux's apic_lvt_mask. */
#define LVT_MASK  (APIC_LVT_MASKED | APIC_SEND_PENDING | APIC_VECTOR_MASK)
#define LINT_MASK (LVT_MASK | APIC_MODE_MASK | APIC_INPUT_POL | APIC_REMOTE_IRR | APIC_LEVEL_TRIG)

/* The timer's modes in LVTT bits 18:17. */
#define TIMER_ONESHOT  0x00000u
#define TIMER_PERIODIC 0x20000u
#define TIMER_DEADLINE 0x40000u

/* The x2APIC ICR's reserved bits, Linux's X2APIC_ICR_RESERVED_BITS. */
#define X2APIC_ICR_RESERVED 0xFFF32000ull

/* The shortest period of a periodic timer, Linux's min_timer_period_us. */
#define MIN_PERIOD_NS 200000ull

/* The longest span a halted vCPU's wait for a TSC deadline is computed
 * for at once; a later deadline is reached by waiting again. */
#define MAX_WAIT_NS 1000000000000ull

#define MSR_IA32_APICBASE     0x01Bu
#define MSR_IA32_TSC_DEADLINE 0x6E0u
#define MSR_X2APIC_FIRST      0x800u
#define MSR_X2APIC_LAST       0x8FFu

#define APIC_BASE_BSP    0x100ull
#define APIC_BASE_X2APIC 0x400ull
#define APIC_BASE_ENABLE 0x800ull
#define APIC_BASE_ADDR   0xFFFFFFFFFF000ull

/* The events another processor leaves, in `events`, and the SIPI that
 * KVM_SET_MP_STATE leaves, whose vector is the one KVM_SET_VCPU_EVENTS
 * gave the vCPU. */
#define EV_INIT     1u
#define EV_SIPI     2u
#define EV_SIPI_SET 4u

#define RFLAGS_IF 0x200ull

/* The leaf 1 ECX bits of the guest's CPUID the APIC reads. */
#define CPUID_X2APIC   (1u << 21)
#define CPUID_DEADLINE (1u << 24)

/*
 * A vCPU's local APIC. `regs` is the register page as the guest reads it,
 * but for LVR, which follows the guest's CPUID, and TMCCT, which the timer
 * gives; in x2APIC mode ID holds the whole x2APIC id. Another processor's
 * delivery (`accept`), such as an IPI or `kvm_lapic_pit_nmi` under the
 * controllers' lock, sets IRR and TMR, adds to `nmis` and sets `events`
 * by atomic operations, and stores `sipi_vector` before EV_SIPI and
 * `pv_unhalted`; the vCPU's own thread takes each of these by an exchange
 * or clears it. The rest change on the vCPU's own thread, under its mutex,
 * as Linux's kvm_lapic changes in the vCPU's thread without kvm->lock, but
 * for TPR, which the processor writes itself while the guest runs under
 * VMX's TPR shadow. The page is a page of its own, at `regs_pa`, since the
 * shadow takes it as the virtual-APIC page, as Linux's kvm_create_lapic
 * gives `apic->regs` a page.
 */
struct lapic {
    volatile uint32_t *regs; /* KVM_APIC_REG_SIZE bytes at the start of `page` */
    void *page;              /* the page, as `kalloc_pages` gave it */
    uintptr_t regs_pa;       /* the page's physical address */
    struct kvm_vcpu *v;
    uint32_t divide;          /* the timer's divisor, 1 to 128 */
    uint32_t timer_mode;      /* TIMER_*, as last applied */
    uint64_t period_ns;       /* the one-shot or periodic span, 0 when stopped */
    uint64_t expire_ns;       /* the host's ns of the next expiry, 0 for none */
    uint64_t tscdeadline;     /* the guest's TSC of the deadline, 0 for none */
    volatile uint32_t events; /* EV_*, from another processor */
    /* The vector of EV_SIPI, from another processor. It is kept here,
     * where an INIT's reset of the vCPU does not reach it: the SIPI may
     * come while its INIT waits or is being taken. */
    volatile uint32_t sipi_vector;
    volatile uint32_t nmis; /* NMIs delivered, not yet queued on the vCPU */
    /* KVM_HC_KICK_CPU's wake, from another processor: the halted vCPU runs
     * again whatever its IF, as Linux's pv_unhalted. */
    volatile uint32_t pv_unhalted;
    int ext_vector;      /* KVM_INTERRUPT's vector, -1 for none */
    uint32_t eoi_vector; /* the vector of KVM_EXIT_IOAPIC_EOI */
    /* The vectors whose EOI goes to the caller's I/O APIC, a bit each, as
     * Linux's ioapic_handled_vectors: found again before an entry after
     * the routes or this APIC's destination change (KVM_REQ_SCAN_IOAPIC). */
    uint32_t eoi_exit[8];
    uint64_t wake_tick; /* the tick a halted wait ends at for the timer, 0 for none */
    wait_queue_t wq;    /* the vCPU's halted wait */
};

/** The APIC of `v`, or NULL with no local APIC in the kernel. */
static struct lapic *apic_of(const struct kvm_vcpu *v)
{
    return (struct lapic *)v->lapic;
}

/** The register at `off`. */
static uint32_t reg_get(const struct lapic *l, uint32_t off)
{
    return l->regs[off >> 2];
}

/** Set the register at `off`. */
static void reg_set(struct lapic *l, uint32_t off, uint32_t value)
{
    l->regs[off >> 2] = value;
}

/** OR `bits` into `*w` atomically. */
static void atomic_or(volatile uint32_t *w, uint32_t bits)
{
    uint32_t old;

    do {
        old = *w;
    } while (arch_atomic_cmpxchg((volatile unsigned int *)w, old, old | bits) != old);
}

/** Clear `bits` in `*w` atomically. */
static void atomic_andnot(volatile uint32_t *w, uint32_t bits)
{
    uint32_t old;

    do {
        old = *w;
    } while (arch_atomic_cmpxchg((volatile unsigned int *)w, old, old & ~bits) != old);
}

/** Add 1 to `*w` atomically. */
static void atomic_inc(volatile uint32_t *w)
{
    uint32_t old;

    do {
        old = *w;
    } while (arch_atomic_cmpxchg((volatile unsigned int *)w, old, old + 1u) != old);
}

/** Set the bit of `vector` in the eight-word vector register at `off`. */
static void vec_set(struct lapic *l, uint32_t off, uint32_t vector)
{
    atomic_or(&l->regs[(off >> 2) + (vector >> 5) * 4u], 1u << (vector & 31u));
}

/** Clear the bit of `vector` in the vector register at `off`. */
static void vec_clear(struct lapic *l, uint32_t off, uint32_t vector)
{
    atomic_andnot(&l->regs[(off >> 2) + (vector >> 5) * 4u], 1u << (vector & 31u));
}

/** The highest vector set in the vector register at `off`, or -1. */
static int vec_highest(const struct lapic *l, uint32_t off)
{
    int i, b;
    uint32_t w;

    for (i = 7; i >= 0; i--) {
        w = l->regs[(off >> 2) + (uint32_t)i * 4u];
        if (w) {
            for (b = 31; b >= 0; b--) {
                if ((w >> b) & 1u) {
                    return i * 32 + b;
                }
            }
        }
    }
    return -1;
}

/** Leaf 1 ECX of `v`'s CPUID, 0 without one. */
static uint32_t guest_l1_ecx(const struct kvm_vcpu *v)
{
    const struct kvm_cpuid_entry2 *e = kvm_cpuid_find(v, 1, 0);

    return e ? e->ecx : 0;
}

/** The version register of `v`'s APIC. */
static uint32_t lvr_of(const struct kvm_vcpu *v)
{
    return APIC_VERSION | ((guest_l1_ecx(v) & CPUID_X2APIC) ? APIC_LVR_DIRECTED : 0);
}

/** The LVTT bits that select the timer's mode: TSC-deadline mode only
 * where the guest's CPUID offers it, as Linux's timer_mode_mask. */
static uint32_t timer_mode_mask(const struct kvm_vcpu *v)
{
    return (guest_l1_ecx(v) & CPUID_DEADLINE) ? 0x60000u : 0x20000u;
}

/** Whether the APIC is enabled in IA32_APIC_BASE. */
static int hw_enabled(const struct lapic *l)
{
    return (l->v->sregs.apic_base & APIC_BASE_ENABLE) != 0;
}

/** Whether the APIC is in x2APIC mode. */
static int x2apic(const struct lapic *l)
{
    return (l->v->sregs.apic_base & APIC_BASE_X2APIC) != 0;
}

/** Whether the APIC is enabled in SPIV. */
static int sw_enabled(const struct lapic *l)
{
    return (reg_get(l, APIC_SPIV) & APIC_SPIV_ENABLED) != 0;
}

/** The x2APIC LDR of the id `id`: its cluster and its bit in it. */
static uint32_t x2apic_ldr(uint32_t id)
{
    return ((id >> 4) << 16) | (1u << (id & 0xFu));
}

/** Leave the request `request` for `v` and wake it from a halted wait. */
static void lapic_kick(struct lapic *l, uint32_t request)
{
    kvm_vcpu_kick(l->v, request);
    wq_wake(&l->wq, 0);
}

void kvm_lapic_wake(struct kvm_vcpu *v, uint32_t request)
{
    struct lapic *l = apic_of(v);

    if (l) {
        lapic_kick(l, request);
    }
}

/** Bring PPR up to date from TPR and the highest vector in service:
 * whether it fell, which may let a waiting interrupt through. */
static int update_ppr(struct lapic *l)
{
    uint32_t tpr = reg_get(l, APIC_TASKPRI), old = reg_get(l, APIC_PROCPRI), ppr;
    int isr = vec_highest(l, APIC_ISR);
    uint32_t isrv = isr < 0 ? 0u : (uint32_t)isr;

    ppr = (tpr & 0xF0u) >= (isrv & 0xF0u) ? tpr & 0xFFu : isrv & 0xF0u;
    reg_set(l, APIC_PROCPRI, ppr);
    return ppr < old;
}

/** Set TPR, and ask for an injection when PPR falls. */
static void set_tpr(struct lapic *l, uint32_t tpr)
{
    reg_set(l, APIC_TASKPRI, tpr & 0xFFu);
    if (update_ppr(l)) {
        kvm_vcpu_kick(l->v, KVM_REQ_EVENT);
    }
}

/* The timer. */

/** The guest's TSC now. */
static uint64_t guest_tsc(const struct kvm_vcpu *v)
{
    return kvm_tsc_read(v);
}

/** The divisor TDCR gives: 1, or 2 to 128 by powers of two. */
static void update_divide(struct lapic *l)
{
    uint32_t t = reg_get(l, APIC_TDCR) & 0xFu;
    uint32_t n = ((t & 0x3u) | ((t & 0x8u) >> 1)) + 1u;

    l->divide = 1u << (n & 7u);
}

/** The ns of one tick of the timer: the divisor's bus cycles, each the
 * VM's `apic_bus_ns`, 1 unless KVM_CAP_X86_APIC_BUS_CYCLES_NS set another. */
static uint64_t tick_ns(const struct lapic *l)
{
    return l->divide * l->v->vm->apic_bus_ns;
}

/** The span in ns of `count` ticks of the timer. */
static uint64_t count_ns(const struct lapic *l, uint32_t count)
{
    return (uint64_t)count * tick_ns(l);
}

/** Stop the one-shot or periodic count. */
static void timer_cancel(struct lapic *l)
{
    l->expire_ns = 0;
}

/**
 * Start the one-shot or periodic count from TMICT, the next expiry after
 * the count in the register `count_reg` (TMICT, or TMCCT when KVM_SET_LAPIC
 * restores it), as Linux's set_target_expiration: a count past the period
 * is the period, and a restored count of 0 expires at once in one-shot mode
 * and after a period in periodic mode.
 */
static void timer_start(struct lapic *l, uint32_t count_reg)
{
    uint64_t span;

    l->period_ns = count_ns(l, reg_get(l, APIC_TMICT));
    if (l->period_ns == 0) {
        l->expire_ns = 0;
        return;
    }
    if (l->timer_mode == TIMER_PERIODIC && l->period_ns < MIN_PERIOD_NS) {
        l->period_ns = MIN_PERIOD_NS;
    }
    span = l->period_ns;
    if (count_reg != APIC_TMICT) {
        span = count_ns(l, reg_get(l, count_reg));
        if (span == 0) {
            span = l->timer_mode == TIMER_PERIODIC ? l->period_ns : 0;
        } else if (span > l->period_ns) {
            span = l->period_ns;
        }
    }
    l->expire_ns = kvm_host_ns() + span;
    if (l->expire_ns == 0) {
        l->expire_ns = 1;
    }
    kvm_timer_arm(l->v, l->expire_ns);
}

/** Apply LVTT's mode, as Linux's apic_update_lvtt: a change into or out of
 * TSC-deadline mode stops the timer and clears TMICT and the deadline. */
static void update_lvtt(struct lapic *l)
{
    uint32_t mode = reg_get(l, APIC_LVTT) & timer_mode_mask(l->v);

    if (mode == l->timer_mode) {
        return;
    }
    if ((l->timer_mode == TIMER_DEADLINE) != (mode == TIMER_DEADLINE)) {
        timer_cancel(l);
        reg_set(l, APIC_TMICT, 0);
        l->period_ns = 0;
        l->tscdeadline = 0;
    }
    l->timer_mode = mode;
    if (mode == TIMER_PERIODIC && l->period_ns && l->period_ns < MIN_PERIOD_NS) {
        l->period_ns = MIN_PERIOD_NS;
    }
}

/** TMCCT: the count left of the period, 0 when stopped, expired in one-shot
 * mode, or in TSC-deadline mode. */
static uint32_t tmcct(const struct lapic *l)
{
    uint64_t now, left;

    if (l->timer_mode == TIMER_DEADLINE || reg_get(l, APIC_TMICT) == 0 || l->period_ns == 0 ||
        l->expire_ns == 0) {
        return 0;
    }
    now = kvm_host_ns();
    if (l->expire_ns > now) {
        left = (l->expire_ns - now) % l->period_ns;
    } else if (l->timer_mode == TIMER_PERIODIC) {
        left = l->period_ns - (now - l->expire_ns) % l->period_ns;
    } else {
        return 0;
    }
    return (uint32_t)(left / tick_ns(l));
}

/** Put the fixed `vector` into `l`'s IRR, with the trigger mode `trig` in
 * TMR, while the APIC is enabled, as Linux's __apic_accept_irq does for a
 * fixed interrupt, but with no kick: 1 when IRR took it, else 0. Safe from
 * any context. */
static int set_irr(struct lapic *l, uint32_t vector, uint32_t trig)
{
    if (!hw_enabled(l) || !sw_enabled(l)) {
        l->v->irq_counts[vector & 0xFFu][KVM_VEC_DROP]++;
        return 0;
    }
    if (trig) {
        vec_set(l, APIC_TMR, vector);
    } else {
        vec_clear(l, APIC_TMR, vector);
    }
    vec_set(l, APIC_IRR, vector);
    l->v->irq_counts[vector & 0xFFu][KVM_VEC_ACCEPT]++;
    return 1;
}

/**
 * Accept one interrupt into `l`, as Linux's __apic_accept_irq: a fixed or
 * lowest-priority vector into IRR with its trigger mode in TMR, while the
 * APIC is enabled; an NMI, INIT or SIPI left for the vCPU's thread. 1 when
 * the APIC took it, else 0. Safe from any context.
 */
static int accept(struct lapic *l, uint32_t mode, uint32_t vector, uint32_t level, uint32_t trig)
{
    switch (mode) {
    case KVM_APIC_DM_LOWEST:
    case KVM_APIC_DM_FIXED:
        if (trig && !level) {
            return 0;
        }
        if (!set_irr(l, vector, trig)) {
            return 0;
        }
        lapic_kick(l, KVM_REQ_EVENT);
        return 1;
    case KVM_APIC_DM_REMRD:
        l->pv_unhalted = 1;
        lapic_kick(l, KVM_REQ_EVENT);
        return 1;
    case KVM_APIC_DM_NMI:
        atomic_inc(&l->nmis);
        lapic_kick(l, KVM_REQ_NMI | KVM_REQ_EVENT);
        return 1;
    case KVM_APIC_DM_INIT:
        if (trig && !level) {
            return 0;
        }
        arch_atomic_xchg((volatile unsigned int *)&l->events, EV_INIT);
        lapic_kick(l, KVM_REQ_INIT_SIPI | KVM_REQ_EVENT);
        return 1;
    case KVM_APIC_DM_STARTUP:
        l->sipi_vector = vector;
        atomic_or(&l->events, EV_SIPI);
        lapic_kick(l, KVM_REQ_INIT_SIPI | KVM_REQ_EVENT);
        return 1;
    case KVM_APIC_DM_SMI: /* no SMM is offered */
    case KVM_APIC_DM_EXTINT:
    default:
        return 0;
    }
}

/** Deliver the LVT entry at `off` to its own APIC, as Linux's
 * kvm_apic_local_deliver. 1 when the APIC took it. */
static int local_deliver(struct lapic *l, uint32_t off)
{
    uint32_t r = reg_get(l, off);

    if (!hw_enabled(l) || (r & APIC_LVT_MASKED)) {
        return 0;
    }
    return accept(l, r & APIC_MODE_MASK, r & APIC_VECTOR_MASK, 1, (r & APIC_LEVEL_TRIG) != 0);
}

void kvm_lapic_pmi(struct kvm_vcpu *v)
{
    struct lapic *l = v->lapic;

    if (l) {
        (void)local_deliver(l, APIC_LVTPC);
    }
}

/** Whether the timer's expiry is due, taking it if so: the TSC deadline
 * passed, which disarms it, or the count ran out, the periodic count
 * starting again from where it ended, missed periods counted as one, as
 * Linux's pending count does. */
static int timer_due(struct lapic *l)
{
    uint64_t now, n;

    if (l->timer_mode == TIMER_DEADLINE) {
        if (l->tscdeadline && guest_tsc(l->v) >= l->tscdeadline) {
            l->tscdeadline = 0;
            return 1;
        }
        return 0;
    }
    if (l->expire_ns == 0) {
        return 0;
    }
    now = kvm_host_ns();
    if (now < l->expire_ns) {
        return 0;
    }
    if (l->timer_mode == TIMER_PERIODIC && l->period_ns) {
        n = (now - l->expire_ns) / l->period_ns + 1u;
        l->expire_ns += n * l->period_ns;
    } else {
        l->expire_ns = 0;
    }
    return 1;
}

/** Deliver the timer's expiry when it is due. */
static void timer_check(struct lapic *l)
{
    if (timer_due(l)) {
        local_deliver(l, APIC_LVTT);
    }
}

void kvm_lapic_timer_check(struct kvm_vcpu *v)
{
    struct lapic *l = apic_of(v);

    if (l) {
        timer_check(l);
    }
}

int kvm_lapic_timer_expire(struct kvm_vcpu *v)
{
    struct lapic *l = apic_of(v);
    uint32_t r;

    if (!l || !timer_due(l)) {
        return 0;
    }
    r = reg_get(l, APIC_LVTT);
    if ((r & APIC_MODE_MASK) != KVM_APIC_DM_FIXED) {
        /* Not the timer's own mode: the run loop takes it. */
        local_deliver(l, APIC_LVTT);
        return 0;
    }
    if (!hw_enabled(l) || (r & APIC_LVT_MASKED)) {
        return 0;
    }
    return set_irr(l, r & APIC_VECTOR_MASK, (r & APIC_LEVEL_TRIG) != 0);
}

/** The host's ns at which the timer next expires, 0 for none. A TSC
 * deadline is turned into ns at the rate of the vCPU's TSC, as Linux
 * does, or taken as one tick away when no rate is known. */
static uint64_t timer_next_ns(const struct lapic *l)
{
    uint64_t now, tsc, left;
    uint32_t khz;

    if (l->timer_mode != TIMER_DEADLINE) {
        return l->expire_ns;
    }
    if (l->tscdeadline == 0) {
        return 0;
    }
    now = kvm_host_ns();
    tsc = guest_tsc(l->v);
    if (tsc >= l->tscdeadline) {
        return now;
    }
    khz = l->v->tsc_khz;
    if (khz == 0) {
        return now + 1000000u;
    }
    left = l->tscdeadline - tsc;
    if (left / khz >= MAX_WAIT_NS / 1000000u) {
        return now + MAX_WAIT_NS;
    }
    return now + left * 1000000u / khz;
}

uint64_t kvm_lapic_run_bound(struct kvm_vcpu *v, uint64_t max)
{
    uint64_t next = kvm_lapic_next_ns(v), now, khz;

    if (!next) {
        return max;
    }
    now = kvm_host_ns();
    if (next <= now) {
        return 0;
    }
    khz = kvm_tsc_khz();
    if (khz == 0 || next - now >= max * 1000000u / khz) {
        return max;
    }
    return (next - now) * khz / 1000000u;
}

/** IA32_TSC_DEADLINE written: in TSC-deadline mode, the new deadline,
 * 0 disarming it, as Linux's kvm_set_lapic_tscdeadline_msr. */
static void set_tscdeadline(struct lapic *l, uint64_t value)
{
    if (!hw_enabled(l) || l->timer_mode != TIMER_DEADLINE) {
        return;
    }
    l->tscdeadline = value;
    kvm_timer_arm(l->v, timer_next_ns(l));
}

/* Delivery. */

/** Whether `l`, in physical destination mode, is the destination `mda`. */
static int match_physical(const struct lapic *l, uint32_t mda)
{
    if (mda == (x2apic(l) ? X2APIC_BROADCAST : APIC_BROADCAST)) {
        return 1;
    }
    if (x2apic(l) || mda > 0xFFu) {
        return mda == (x2apic(l) ? reg_get(l, APIC_ID) : l->v->id);
    }
    return mda == reg_get(l, APIC_ID) >> 24;
}

/** Whether `l`, in logical destination mode, is in the destination `mda`. */
static int match_logical(const struct lapic *l, uint32_t mda)
{
    uint32_t ldr;

    if (mda == (x2apic(l) ? X2APIC_BROADCAST : APIC_BROADCAST)) {
        return 1;
    }
    ldr = reg_get(l, APIC_LDR);
    if (x2apic(l)) {
        return (ldr >> 16) == (mda >> 16) && (ldr & mda & 0xFFFFu) != 0;
    }
    ldr >>= 24;
    switch (reg_get(l, APIC_DFR)) {
    case APIC_DFR_FLAT:
        return (ldr & mda) != 0;
    case APIC_DFR_CLUSTER:
        return (ldr >> 4) == (mda >> 4) && (ldr & mda & 0xFu) != 0;
    default:
        return 0;
    }
}

/** Match a physical or logical destination for restored IOAPIC state. */
int kvm_lapic_match_dest(struct kvm_vcpu *v, uint32_t id, int logical)
{
    struct lapic *l = apic_of(v);

    return l && hw_enabled(l) && (logical ? match_logical(l, id) : match_physical(l, id));
}

/** Whether `target` is a destination of `irq` sent by `src`, as Linux's
 * kvm_apic_match_dest; a device's broadcast to an x2APIC is x2APIC's,
 * unless KVM_X2APIC_API_DISABLE_BROADCAST_QUIRK makes 0xFF an x2APIC id. */
static int match_dest(const struct lapic *target, const struct lapic *src,
                      const struct kvm_lapic_irq *irq)
{
    uint32_t mda = irq->dest_id;

    if (!src && mda == APIC_BROADCAST && x2apic(target) && !target->v->vm->x2apic_no_broadcast) {
        mda = X2APIC_BROADCAST;
    }
    switch (irq->shorthand) {
    case 0:
        return irq->dest_mode ? match_logical(target, mda) : match_physical(target, mda);
    case 1:
        return target == src;
    case 2:
        return 1;
    case 3:
        return target != src;
    default:
        return 0;
    }
}

int kvm_lapic_match_irq(struct kvm_vcpu *v, const struct kvm_lapic_irq *irq)
{
    struct lapic *l = apic_of(v);

    return l && irq->shorthand == 0 && match_dest(l, NULL, irq);
}

int kvm_lapic_deliver_tracked(struct kvm_vm *vm, struct kvm_vcpu *src,
                              const struct kvm_lapic_irq *in, uint64_t *destinations)
{
    struct kvm_lapic_irq irq = *in;
    struct lapic *s = src ? apic_of(src) : NULL;
    struct lapic *l;
    uint32_t i, n, dests = 0;
    uint64_t chosen[KVM_VCPU_WORDS];
    int lowest, naccepted, r = -1;

    k_memset(chosen, 0, sizeof(chosen));
    if (destinations) {
        k_memset(destinations, 0, KVM_VCPU_WORDS * sizeof(*destinations));
    }

    lowest = irq.delivery_mode == KVM_APIC_DM_LOWEST || irq.redir_hint;
    if (!irq.dest_mode && irq.dest_id == APIC_BROADCAST && lowest) {
        irq.delivery_mode = KVM_APIC_DM_FIXED;
        irq.redir_hint = 0;
        lowest = 0;
    }
    n = vm->nr_vcpus;
    for (i = 0; i < n; i++) {
        l = apic_of(vm->vcpus[i]);
        if (!l || !hw_enabled(l) || !match_dest(l, s, &irq)) {
            continue;
        }
        if (!lowest) {
            if (r < 0) {
                r = 0;
            }
            naccepted = accept(l, irq.delivery_mode, irq.vector, irq.level, irq.trig_mode);
            r += naccepted;
            if (naccepted && destinations) {
                KVM_VCPU_SET(destinations, i);
            }
        } else if (sw_enabled(l)) {
            KVM_VCPU_SET(chosen, i);
            dests++;
        }
    }
    if (dests) {
        /* Linux's vector hashing: the (vector mod count)th destination. */
        uint32_t k = irq.vector % dests;

        for (i = 0; i < n; i++) {
            if (KVM_VCPU_TEST(chosen, i) && k-- == 0) {
                r = accept(apic_of(vm->vcpus[i]), irq.delivery_mode, irq.vector, irq.level,
                           irq.trig_mode);
                if (r && destinations) {
                    KVM_VCPU_SET(destinations, i);
                }
                break;
            }
        }
    }
    return r;
}

/** Deliver without retaining the destination set. */
int kvm_lapic_deliver(struct kvm_vm *vm, struct kvm_vcpu *src, const struct kvm_lapic_irq *irq)
{
    return kvm_lapic_deliver_tracked(vm, src, irq, NULL);
}

/** Whether the vector is pending or in service, for RTC EOI tracking. */
int kvm_lapic_pending_eoi(struct kvm_vcpu *v, uint32_t vector)
{
    struct lapic *l = apic_of(v);
    uint32_t word = (vector >> 5) * 4u, bit = 1u << (vector & 31u);

    return l && vector < KVM_NR_INTERRUPTS &&
           ((l->regs[(APIC_IRR >> 2) + word] | l->regs[(APIC_ISR >> 2) + word]) & bit) != 0;
}

/** The PIT's virtual-wire NMI, as Linux's kvm_apic_nmi_wd_deliver. */
void kvm_lapic_pit_nmi(struct kvm_vm *vm)
{
    struct lapic *l;
    uint32_t i, lvt;

    for (i = 0; i < vm->nr_vcpus; i++) {
        l = apic_of(vm->vcpus[i]);
        if (!l) {
            continue;
        }
        lvt = reg_get(l, APIC_LVT0);
        if (!(lvt & APIC_LVT_MASKED) && (lvt & APIC_MODE_MASK) == KVM_APIC_DM_NMI) {
            accept(l, KVM_APIC_DM_NMI, 0, 1, 0);
        }
    }
}

/** Deliver `irq` to each vCPU whose physical APIC id is `min` plus a bit
 * set in `bitmap`'s low `bits`, as Linux's __pv_send_ipi: how many took
 * it. */
static int pv_ipi_cluster(struct kvm_vm *vm, uint64_t bitmap, uint32_t min, uint32_t bits,
                          const struct kvm_lapic_irq *irq)
{
    struct lapic *l;
    uint32_t i, k, id;
    int count = 0;

    for (i = 0; i < bits; i++) {
        if (!(bitmap & (1ull << i))) {
            continue;
        }
        for (k = 0; k < vm->nr_vcpus; k++) {
            l = apic_of(vm->vcpus[k]);
            if (!l) {
                continue;
            }
            id = x2apic(l) ? reg_get(l, APIC_ID) : reg_get(l, APIC_ID) >> 24;
            if (id == min + i) {
                count += accept(l, irq->delivery_mode, irq->vector, irq->level, irq->trig_mode);
                break;
            }
        }
    }
    return count;
}

int64_t kvm_lapic_pv_ipi(struct kvm_vm *vm, uint64_t low, uint64_t high, uint32_t min, uint64_t icr,
                         uint32_t bits)
{
    struct kvm_lapic_irq irq;
    uint32_t i, top = 0;
    struct lapic *l;
    int count = 0;

    if (icr & (APIC_DEST_LOGICAL | APIC_SHORT_MASK)) {
        return -KVM_EINVAL;
    }
    irq.vector = (uint32_t)icr & APIC_VECTOR_MASK;
    irq.delivery_mode = (uint32_t)icr & APIC_MODE_MASK;
    irq.dest_mode = 0;
    irq.level = (icr & APIC_INT_ASSERT) != 0;
    irq.trig_mode = (icr & APIC_LEVEL_TRIG) != 0;
    irq.shorthand = 0;
    irq.redir_hint = 0;
    irq.dest_id = 0;
    /* Linux reads each half only while `min` is an id some vCPU may have,
     * and lets the second half's `min` wrap as a 32-bit sum does. */
    for (i = 0; i < vm->nr_vcpus; i++) {
        l = apic_of(vm->vcpus[i]);
        if (l) {
            uint32_t id = x2apic(l) ? reg_get(l, APIC_ID) : reg_get(l, APIC_ID) >> 24;

            top = id > top ? id : top;
        }
    }
    if (min <= top) {
        count = pv_ipi_cluster(vm, low, min, bits, &irq);
    }
    min += bits;
    if (min <= top) {
        count += pv_ipi_cluster(vm, high, min, bits, &irq);
    }
    return count;
}

/** Send the IPI the ICR names, `low` and `high`, as Linux's
 * kvm_apic_send_ipi. */
static void send_ipi(struct lapic *l, uint32_t low, uint32_t high)
{
    struct kvm_lapic_irq irq;

    irq.vector = low & APIC_VECTOR_MASK;
    irq.delivery_mode = low & APIC_MODE_MASK;
    irq.dest_mode = (low & APIC_DEST_LOGICAL) != 0;
    irq.level = (low & APIC_INT_ASSERT) != 0;
    irq.trig_mode = (low & APIC_LEVEL_TRIG) != 0;
    irq.shorthand = (low & APIC_SHORT_MASK) >> APIC_SHORT_SHIFT;
    irq.redir_hint = 0;
    irq.dest_id = x2apic(l) ? high : high >> 24;
    kvm_lapic_deliver(l->v->vm, l->v, &irq);
}

/** An EOI, as Linux's apic_set_eoi: the highest vector in service ends,
 * a SynIC's SINT of it is acknowledged, and when the caller's I/O APIC
 * routes it level-triggered to this vCPU, or did when it was pending
 * here (`eoi_exit`), the vCPU goes to the caller with
 * KVM_EXIT_IOAPIC_EOI, as Linux's kvm_ioapic_send_eoi
 * (arch/x86/kvm/lapic.c:1463-1482). */
static void set_eoi(struct lapic *l)
{
    int vector = vec_highest(l, APIC_ISR);

    if (vector < 0) {
        return;
    }
    l->regs[(APIC_ISR >> 2) + ((uint32_t)vector >> 5) * 4u] &= ~(1u << ((uint32_t)vector & 31u));
    l->v->irq_counts[vector][KVM_VEC_EOI]++;
    update_ppr(l);
    kvm_hv_synic_eoi(l->v, (uint32_t)vector);
    kvm_ioapic_eoi(l->v, (uint32_t)vector,
                   (l->regs[(APIC_TMR >> 2) + ((uint32_t)vector >> 5) * 4u] &
                    (1u << ((uint32_t)vector & 31u))) != 0,
                   (reg_get(l, APIC_SPIV) & APIC_SPIV_DIRECTED) != 0);
    if ((l->eoi_exit[(uint32_t)vector >> 5] >> ((uint32_t)vector & 31u)) & 1u) {
        l->eoi_vector = (uint32_t)vector;
        kvm_vcpu_kick(l->v, KVM_REQ_EOI_EXIT);
    }
    kvm_vcpu_kick(l->v, KVM_REQ_EVENT);
}

/* The registers. */

/** The value of the register at `off`, the 16-byte aligned one, as
 * Linux's __apic_read. */
static uint32_t apic_read(struct lapic *l, uint32_t off)
{
    switch (off) {
    case APIC_ARBPRI:
        return 0;
    case APIC_LVR:
        return lvr_of(l->v);
    case APIC_TMCCT:
        return tmcct(l);
    case APIC_PROCPRI:
        update_ppr(l);
        return reg_get(l, off);
    default:
        return reg_get(l, off);
    }
}

/** Read `len` bytes at `off` into `*out`, as Linux's kvm_lapic_reg_read:
 * 0, or 1 for a read across registers or of a register not readable in
 * this mode. */
static int reg_read(struct lapic *l, uint32_t off, uint32_t len, uint32_t *out)
{
    uint64_t readable = READABLE | (x2apic(l) ? 0 : READABLE_XAPIC);
    uint32_t align = off & 0xFu;

    if (align + len > 4u || off > 0x3F0u || !(readable & REG_BIT(off))) {
        return 1;
    }
    *out = apic_read(l, off & ~0xFu) >> (align * 8u);
    if (len < 4u) {
        *out &= (1u << (len * 8u)) - 1u;
    }
    return 0;
}

/** Mask every LVT entry, as clearing SPIV's enable does. */
static void mask_lvts(struct lapic *l)
{
    uint32_t i;

    for (i = 0; i < APIC_NR_LVT; i++) {
        reg_set(l, APIC_LVTT + i * 0x10u, reg_get(l, APIC_LVTT + i * 0x10u) | APIC_LVT_MASKED);
    }
}

/** Write TDCR: a new divisor scales what is left of a running count, as
 * Linux's update_target_expiration. */
static void set_tdcr(struct lapic *l, uint32_t val)
{
    uint32_t old = l->divide;
    uint64_t now, left;

    reg_set(l, APIC_TDCR, val & 0xBu);
    update_divide(l);
    if (l->divide == old || !l->period_ns || !l->expire_ns) {
        return;
    }
    now = kvm_host_ns();
    left = l->expire_ns > now ? l->expire_ns - now : 0;
    l->period_ns = count_ns(l, reg_get(l, APIC_TMICT));
    if (l->timer_mode == TIMER_PERIODIC && l->period_ns < MIN_PERIOD_NS) {
        l->period_ns = MIN_PERIOD_NS;
    }
    l->expire_ns = now + left / old * l->divide + 1u;
    kvm_timer_arm(l->v, l->expire_ns);
}

/** Have `l`'s vCPU find its EOI exits again before its next entry, since
 * the destinations it answers to changed, as Linux's
 * kvm_recalculate_apic_map asks every vCPU (arch/x86/kvm/lapic.c:494). */
static void dest_changed(struct lapic *l)
{
    kvm_vcpu_kick(l->v, KVM_REQ_SCAN_IOAPIC);
}

/** Whether LINT0 takes the caller's ExtINT, as Linux's
 * kvm_apic_accept_pic_intr: with the APIC off, or LINT0 unmasked in
 * ExtINT mode. */
static int accept_pic_intr(const struct lapic *l)
{
    uint32_t lvt0 = reg_get(l, APIC_LVT0);

    if (!hw_enabled(l)) {
        return 1;
    }
    return !(lvt0 & APIC_LVT_MASKED) && (lvt0 & APIC_MODE_MASK) == KVM_APIC_DM_EXTINT;
}

/** Have `l`'s vCPU look for an interrupt to inject (KVM_REQ_EVENT) when
 * its LINT0, which refused the PIC's INT before a change of LVT0 or of the
 * APIC's enable, takes it now: the PIC woke no vCPU for an INT that no
 * LINT0 took (`kvm_lapic_pic_wake`), so the INT would wait for another
 * event. In the vCPU's thread or under its mutex. */
static void pic_reaccepted(struct lapic *l)
{
    if (accept_pic_intr(l)) {
        kvm_vcpu_kick(l->v, KVM_REQ_EVENT);
    }
}

/** Write `val` to the register at `off`, as Linux's kvm_lapic_reg_write:
 * 0, or 1 for a register this mode does not let the guest write, which an
 * x2APIC MSR write takes as #GP. */
static int reg_write(struct lapic *l, uint32_t off, uint32_t val)
{
    uint32_t mask;
    int pic;

    switch (off) {
    case APIC_ID:
        if (x2apic(l)) {
            return 1;
        }
        reg_set(l, APIC_ID, val & 0xFF000000u);
        dest_changed(l);
        return 0;
    case APIC_TASKPRI:
        set_tpr(l, val);
        return 0;
    case APIC_EOI:
        set_eoi(l);
        return 0;
    case APIC_LDR:
        if (x2apic(l)) {
            return 1;
        }
        reg_set(l, APIC_LDR, val & APIC_LDR_MASK);
        dest_changed(l);
        return 0;
    case APIC_DFR:
        if (x2apic(l)) {
            return 1;
        }
        reg_set(l, APIC_DFR, val | APIC_DFR_CLUSTER);
        dest_changed(l);
        return 0;
    case APIC_SPIV:
        mask = 0x3FFu | ((lvr_of(l->v) & APIC_LVR_DIRECTED) ? APIC_SPIV_DIRECTED : 0);
        reg_set(l, APIC_SPIV, val & mask);
        if (!(val & APIC_SPIV_ENABLED)) {
            mask_lvts(l);
            update_lvtt(l);
        }
        return 0;
    case APIC_ICR:
        if (x2apic(l)) {
            return 1;
        }
        val &= ~APIC_SEND_PENDING;
        send_ipi(l, val, reg_get(l, APIC_ICR2));
        reg_set(l, APIC_ICR, val);
        return 0;
    case APIC_ICR2:
        if (x2apic(l)) {
            return 1;
        }
        reg_set(l, APIC_ICR2, val & 0xFF000000u);
        return 0;
    case APIC_LVTTHMR:
    case APIC_LVTPC:
    case APIC_LVT0:
    case APIC_LVT1:
    case APIC_LVTERR:
        if (!sw_enabled(l)) {
            val |= APIC_LVT_MASKED;
        }
        mask = (off == APIC_LVT0 || off == APIC_LVT1) ? LINT_MASK
               : (off == APIC_LVTERR)                 ? LVT_MASK
                                                      : LVT_MASK | APIC_MODE_MASK;
        pic = off == APIC_LVT0 && !accept_pic_intr(l);
        reg_set(l, off, val & mask);
        if (pic) {
            pic_reaccepted(l);
        }
        return 0;
    case APIC_LVTT:
        if (!sw_enabled(l)) {
            val |= APIC_LVT_MASKED;
        }
        reg_set(l, APIC_LVTT, val & (LVT_MASK | timer_mode_mask(l->v)));
        update_lvtt(l);
        return 0;
    case APIC_TMICT:
        if (l->timer_mode == TIMER_DEADLINE) {
            return 0;
        }
        timer_cancel(l);
        reg_set(l, APIC_TMICT, val);
        timer_start(l, APIC_TMICT);
        return 0;
    case APIC_TDCR:
        set_tdcr(l, val);
        return 0;
    case APIC_ESR:
        return x2apic(l) && val != 0;
    case APIC_SELF_IPI:
        if (!x2apic(l) || (val & ~APIC_VECTOR_MASK)) {
            return 1;
        }
        send_ipi(l, (1u << APIC_SHORT_SHIFT) | val, 0);
        return 0;
    default:
        return 1;
    }
}

/** Reset `l` as Linux's kvm_lapic_reset, for a vCPU's creation or, with
 * `init`, an INIT, which keeps the APIC's mode and its id. The boot vCPU's
 * LINT0 takes ExtINT, as Linux's LINT0_REENABLED quirk sets it until the
 * caller turns it off, so the caller's PIC reaches it before the guest
 * programs its APIC. */
static void lapic_reset(struct lapic *l, int init)
{
    struct kvm_vcpu *v = l->v;
    uint32_t i;

    timer_cancel(l);
    if (!init) {
        reg_set(l, APIC_ID, x2apic(l) ? v->id : v->id << 24);
    }
    for (i = 0; i < APIC_NR_LVT; i++) {
        reg_set(l, APIC_LVTT + i * 0x10u, APIC_LVT_MASKED);
    }
    l->timer_mode = TIMER_ONESHOT;
    l->period_ns = 0;
    l->tscdeadline = 0;
    l->pv_unhalted = 0;
    if (v->id == v->vm->boot_cpu_id && KVM_QUIRK(v->vm, KVM_X86_QUIRK_LINT0_REENABLED)) {
        reg_set(l, APIC_LVT0, KVM_APIC_DM_EXTINT);
    }
    reg_set(l, APIC_DFR, APIC_DFR_FLAT);
    reg_set(l, APIC_SPIV, 0xFFu);
    reg_set(l, APIC_TASKPRI, 0);
    if (!x2apic(l)) {
        reg_set(l, APIC_LDR, 0);
    }
    reg_set(l, APIC_ESR, 0);
    reg_set(l, APIC_ICR, 0);
    reg_set(l, APIC_ICR2, 0);
    reg_set(l, APIC_TDCR, 0);
    reg_set(l, APIC_TMICT, 0);
    for (i = 0; i < 8; i++) {
        reg_set(l, APIC_IRR + i * 0x10u, 0);
        reg_set(l, APIC_ISR + i * 0x10u, 0);
        reg_set(l, APIC_TMR + i * 0x10u, 0);
    }
    update_divide(l);
    l->nmis = 0;
    l->ext_vector = -1;
    reg_set(l, APIC_PROCPRI, 0);
    update_ppr(l);
    dest_changed(l);
}

/* The interface. */

int kvm_lapic_create(struct kvm_vcpu *v)
{
    struct lapic *l = kzalloc(sizeof(*l));

    if (!l) {
        return -ENOMEM;
    }
    l->page = kalloc_pages(1, &l->regs_pa);
    if (!l->page) {
        kfree(l);
        return -ENOMEM;
    }
    k_memset(l->page, 0, 4096);
    l->regs = (volatile uint32_t *)l->page;
    l->v = v;
    wait_queue_init(&l->wq);
    v->lapic = l;
    lapic_reset(l, 0);
    return 0;
}

void kvm_lapic_free(struct kvm_vcpu *v)
{
    struct lapic *l = apic_of(v);

    if (l) {
        kfree_pages(l->page, 1);
        kfree(l);
    }
    v->lapic = NULL;
}

int kvm_lapic_set_base(struct kvm_vcpu *v, uint64_t value, int host)
{
    const struct kvm_cpuid_entry2 *e = kvm_cpuid_find(v, 0x80000000u, 0);
    uint64_t old = v->sregs.apic_base, reserved;
    uint32_t phys = 36;
    struct lapic *l = apic_of(v);
    int pic;

    if (e && e->eax >= 0x80000008u && (e = kvm_cpuid_find(v, 0x80000008u, 0)) != NULL) {
        phys = e->eax & 0xFFu;
    }
    reserved = (~0ull << phys) | 0x2FFull;
    if (!(guest_l1_ecx(v) & CPUID_X2APIC)) {
        reserved |= APIC_BASE_X2APIC;
    }
    if ((value & reserved) || ((value & APIC_BASE_X2APIC) && !(value & APIC_BASE_ENABLE))) {
        return -1;
    }
    if (!host) {
        if ((old & APIC_BASE_X2APIC) && !(value & APIC_BASE_X2APIC) && (value & APIC_BASE_ENABLE)) {
            return -1;
        }
        if (!(old & APIC_BASE_ENABLE) && (value & APIC_BASE_X2APIC)) {
            return -1;
        }
    }
    pic = l && !accept_pic_intr(l);
    v->sregs.apic_base = value;
    v->dirty |= KVM_PART_SREGS;
    if (!l) {
        return 0;
    }
    if (pic) {
        pic_reaccepted(l);
    }
    if (((old ^ value) & APIC_BASE_ENABLE) && (value & APIC_BASE_ENABLE)) {
        reg_set(l, APIC_ID, v->id << 24);
    }
    if ((old ^ value) & APIC_BASE_X2APIC) {
        if (value & APIC_BASE_X2APIC) {
            reg_set(l, APIC_ID, v->id);
            reg_set(l, APIC_LDR, x2apic_ldr(v->id));
        } else if (value & APIC_BASE_ENABLE) {
            reg_set(l, APIC_ID, v->id << 24);
        }
    }
    if ((old ^ value) & (APIC_BASE_ENABLE | APIC_BASE_X2APIC)) {
        dest_changed(l);
    }
    return 0;
}

/** Read or write, by `write`, the register at `off` of `l` as its x2APIC
 * MSR, as Linux's kvm_lapic_msr_read and kvm_lapic_msr_write: the ICR
 * whole, with its reserved bits refused, and any other register in its low
 * 32 bits. 0, or -1 for a value or register refused. */
static int msr_reg(struct lapic *l, uint32_t off, int write, uint64_t *value)
{
    uint32_t low;

    if (!write) {
        if (off == APIC_ICR) {
            *value = reg_get(l, APIC_ICR) | (uint64_t)reg_get(l, APIC_ICR2) << 32;
            return 0;
        }
        if (reg_read(l, off, 4, &low)) {
            return -1;
        }
        *value = low;
        return 0;
    }
    if (off == APIC_ICR) {
        if (*value & X2APIC_ICR_RESERVED) {
            return -1;
        }
        low = (uint32_t)*value & ~APIC_SEND_PENDING;
        send_ipi(l, low, (uint32_t)(*value >> 32));
        reg_set(l, APIC_ICR, low);
        reg_set(l, APIC_ICR2, (uint32_t)(*value >> 32));
        return 0;
    }
    if (*value >> 32) {
        return -1;
    }
    return reg_write(l, off, (uint32_t)*value) ? -1 : 0;
}

int kvm_lapic_msr(struct kvm_vcpu *v, uint32_t index, int write, uint64_t *value, int host)
{
    struct lapic *l = apic_of(v);

    if (index == MSR_IA32_APICBASE) {
        if (!write) {
            *value = v->sregs.apic_base;
            return 0;
        }
        return kvm_lapic_set_base(v, *value, host);
    }
    if (index == MSR_IA32_TSC_DEADLINE) {
        if (!write) {
            *value = l && hw_enabled(l) && l->timer_mode == TIMER_DEADLINE ? l->tscdeadline : 0;
        } else if (l) {
            set_tscdeadline(l, *value);
        }
        return 0;
    }
    if (index < MSR_X2APIC_FIRST || index > MSR_X2APIC_LAST) {
        return 1;
    }
    if (!l || !x2apic(l)) {
        return -1;
    }
    return msr_reg(l, (index - MSR_X2APIC_FIRST) << 4, write, value);
}

int kvm_lapic_hv_msr(struct kvm_vcpu *v, uint32_t reg, int write, uint64_t *value)
{
    struct lapic *l = apic_of(v);

    return !l || msr_reg(l, reg, write, value) != 0;
}

int kvm_lapic_set_irq(struct kvm_vcpu *v, uint32_t vector)
{
    struct lapic *l = apic_of(v);

    return l ? accept(l, KVM_APIC_DM_FIXED, vector, 1, 0) : -1;
}

int kvm_lapic_mmio(struct kvm_vcpu *v)
{
    struct lapic *l = apic_of(v);
    struct kvm_exit *ex = &v->exit;
    uint64_t base, addr = ex->mmio.phys_addr;
    uint32_t off, len = ex->mmio.len, val = 0, i;

    if (!l) {
        return 0;
    }
    base = v->sregs.apic_base & APIC_BASE_ADDR;
    if (addr < base || addr >= base + 0x1000u || len == 0 || len > 8u) {
        return 0;
    }
    off = (uint32_t)(addr - base);
    /* An access through MMIO in x2APIC mode or with the APIC off is dropped,
     * as Linux's LAPIC_MMIO_HOLE quirk drops it; with the quirk off the
     * page is no APIC's and the access goes on to the caller. */
    if ((!hw_enabled(l) || x2apic(l)) && !KVM_QUIRK(v->vm, KVM_X86_QUIRK_LAPIC_MMIO_HOLE)) {
        return 0;
    }
    if (ex->mmio.is_write) {
        /* A register is written only whole, at its start. */
        if (hw_enabled(l) && !x2apic(l) && len == 4u && (off & 0xFu) == 0) {
            for (i = 0; i < 4u; i++) {
                val |= (uint32_t)ex->mmio.data[i] << (i * 8u);
            }
            reg_write(l, off, val);
        }
        return 1;
    }
    if (!hw_enabled(l) || x2apic(l)) {
        k_memset(ex->mmio.data, 0xFF, len);
    } else {
        k_memset(ex->mmio.data, 0, sizeof(ex->mmio.data));
        if (reg_read(l, off, len, &val) == 0) {
            for (i = 0; i < len && i < 4u; i++) {
                ex->mmio.data[i] = (uint8_t)(val >> (i * 8u));
            }
        }
    }
    if (kvm_backend && kvm_backend->complete) {
        kvm_backend->complete(v);
    }
    return 1;
}

uint64_t kvm_lapic_cr8(struct kvm_vcpu *v)
{
    struct lapic *l = apic_of(v);

    return l ? (reg_get(l, APIC_TASKPRI) >> 4) & 0xFu : v->sregs.cr8;
}

void kvm_lapic_set_cr8(struct kvm_vcpu *v, uint64_t cr8)
{
    struct lapic *l = apic_of(v);

    if (l) {
        set_tpr(l, (uint32_t)(cr8 & 0xFu) << 4);
    }
}

uintptr_t kvm_lapic_page(struct kvm_vcpu *v)
{
    struct lapic *l = apic_of(v);

    return l ? l->regs_pa : 0;
}

int kvm_lapic_highest_irr(struct kvm_vcpu *v)
{
    struct lapic *l = apic_of(v);

    if (!l || !hw_enabled(l)) {
        return -1;
    }
    return vec_highest(l, APIC_IRR);
}

void kvm_lapic_update_ppr(struct kvm_vcpu *v)
{
    struct lapic *l = apic_of(v);

    if (l) {
        update_ppr(l);
        kvm_vcpu_kick(v, KVM_REQ_EVENT);
    }
}

/** Wake the first local APIC whose LINT0 accepts the PIC's INT. */
void kvm_lapic_pic_wake(struct kvm_vm *vm)
{
    struct lapic *l;
    uint32_t i;

    for (i = 0; i < vm->nr_vcpus; i++) {
        l = apic_of(vm->vcpus[i]);
        if (l && accept_pic_intr(l)) {
            lapic_kick(l, KVM_REQ_EVENT);
            return;
        }
    }
}

int kvm_lapic_pending(struct kvm_vcpu *v)
{
    struct lapic *l = apic_of(v);
    int irr;

    if (!l) {
        return -1;
    }
    if (accept_pic_intr(l)) {
        if (v->vm->irqchip == KVM_IRQCHIP_KERNEL) {
            irr = kvm_pic_pending(v->vm);
            if (irr >= 0) {
                return irr;
            }
        } else if (l->ext_vector >= 0) {
            return l->ext_vector;
        }
    }
    if (!hw_enabled(l)) {
        return -1;
    }
    update_ppr(l);
    irr = vec_highest(l, APIC_IRR);
    if (irr < 0 || ((uint32_t)irr & 0xF0u) <= reg_get(l, APIC_PROCPRI)) {
        return -1;
    }
    return irr;
}

int kvm_lapic_ack(struct kvm_vcpu *v)
{
    struct lapic *l = apic_of(v);
    int vector = kvm_lapic_pending(v);

    if (vector < 0) {
        return -1;
    }
    if (accept_pic_intr(l)) {
        if (v->vm->irqchip == KVM_IRQCHIP_KERNEL && kvm_pic_pending(v->vm) >= 0) {
            vector = kvm_pic_ack(v->vm);
            if (vector >= 0) {
                v->irq_counts[vector & 0xFF][KVM_VEC_ACK]++;
            }
            return vector;
        }
        if (l->ext_vector >= 0 && v->vm->irqchip == KVM_IRQCHIP_SPLIT) {
            l->ext_vector = -1;
            v->irq_counts[vector & 0xFF][KVM_VEC_ACK]++;
            return vector;
        }
    }
    vec_clear(l, APIC_IRR, (uint32_t)vector);
    v->irq_counts[vector & 0xFF][KVM_VEC_ACK]++;
    if (!kvm_hv_synic_auto_eoi(v, (uint32_t)vector)) {
        l->regs[(APIC_ISR >> 2) + ((uint32_t)vector >> 5) * 4u] |= 1u << ((uint32_t)vector & 31u);
    }
    update_ppr(l);
    return vector;
}

int kvm_lapic_ready(struct kvm_vcpu *v)
{
    struct lapic *l = apic_of(v);

    if (v->vm->irqchip == KVM_IRQCHIP_KERNEL) {
        return 1;
    }
    return l && v->irq_allowed && l->ext_vector < 0 && accept_pic_intr(l) &&
           !v->events.interrupt.injected && !v->events.exception.injected &&
           !v->events.nmi.injected;
}

int kvm_lapic_halt(struct kvm_vcpu *v)
{
    if (!apic_of(v)) {
        return 1;
    }
    /* A pending exception, the single-step trap of the HLT itself among
     * them, keeps the vCPU running, as Linux's kvm_vcpu_has_events. */
    if (v->events.exception.pending) {
        return 0;
    }
    v->mp_state = KVM_MP_STATE_HALTED;
    v->dirty |= KVM_PART_MP;
    return 0;
}

/** Queue the NMIs other processors delivered on the vCPU, as Linux's
 * process_nmi keeps them: at most two waiting, or one while the guest has
 * NMIs blocked or one is being injected, since that one stands for the
 * NMI the processor would hold pending. */
static void take_nmis(struct lapic *l)
{
    struct kvm_vcpu *v = l->v;
    uint32_t n, limit = (v->events.nmi.masked || v->events.nmi.injected) ? 1u : 2u;

    if (l->nmis == 0) {
        return;
    }
    n = arch_atomic_xchg((volatile unsigned int *)&l->nmis, 0);
    n += v->nmi_queued;
    v->nmi_queued = n > limit ? limit : n;
    v->dirty |= KVM_PART_EVENTS;
}

/** Take a waiting INIT or SIPI, as Linux's kvm_apic_accept_events: an INIT
 * resets the vCPU, leaving the boot vCPU RUNNABLE and another waiting for a
 * SIPI; a SIPI starts a waiting vCPU at its vector's page in real mode and
 * is dropped otherwise. */
static void accept_events(struct lapic *l)
{
    struct kvm_vcpu *v = l->v;
    uint32_t ev, vector;

    if (l->events == 0) {
        return;
    }
    ev = arch_atomic_xchg((volatile unsigned int *)&l->events, 0);
    if (ev & EV_INIT) {
        kvm_vcpu_init_reset(v);
        lapic_reset(l, 1);
        v->mp_state = (v->sregs.apic_base & APIC_BASE_BSP) ? KVM_MP_STATE_RUNNABLE
                                                           : KVM_MP_STATE_INIT_RECEIVED;
        v->dirty |= KVM_PART_MP;
    }
    if ((ev & (EV_SIPI | EV_SIPI_SET)) && v->mp_state == KVM_MP_STATE_INIT_RECEIVED) {
        vector = ((ev & EV_SIPI) ? l->sipi_vector : v->events.sipi_vector) & 0xFFu;
        v->sregs.cs.selector = (uint16_t)(vector << 8);
        v->sregs.cs.base = (uint64_t)vector << 12;
        v->regs.rip = 0;
        v->mp_state = KVM_MP_STATE_RUNNABLE;
        v->dirty |= KVM_PART_REGS | KVM_PART_SREGS | KVM_PART_MP;
    }
}

/** Whether the guest takes an interrupt now: RFLAGS.IF as KVM_SET_REGS
 * left it, or as the backend last reported it. */
static int guest_if(const struct kvm_vcpu *v)
{
    return (v->dirty & KVM_PART_REGS) ? (v->regs.rflags & RFLAGS_IF) != 0 : v->if_flag != 0;
}

/** The host's ns at which `l`'s vCPU next has a timer due, the APIC's or a
 * Hyper-V synthetic timer, 0 for none. */
static uint64_t next_ns(const struct lapic *l)
{
    uint64_t a = timer_next_ns(l), b = kvm_hv_stimer_next(l->v);
    uint64_t pit = kvm_pit_next(l->v->vm);

    a = !a || (b && b < a) ? b : a;
    return !a || (pit && pit < a) ? pit : a;
}

uint64_t kvm_lapic_next_ns(struct kvm_vcpu *v)
{
    struct lapic *l = apic_of(v);

    return l ? next_ns(l) : 0;
}

int kvm_lapic_other_due(struct kvm_vcpu *v)
{
    uint64_t now, hv, pit;

    if (!apic_of(v)) {
        return 0;
    }
    hv = kvm_hv_stimer_next(v);
    pit = kvm_pit_next(v->vm);
    if (!hv && !pit) {
        return 0;
    }
    now = kvm_host_ns();
    return (hv && hv <= now) || (pit && pit <= now);
}

/** Whether the halted or waiting vCPU `arg` has an event to wake it, as
 * Linux's kvm_vcpu_has_events, or a request, or its wait's tick came. A
 * timer changed by another thread, the PIT above all, ends the wait
 * (KVM_REQ_TIMER), so that `kvm_lapic_prepare` finds the new deadline and
 * waits again until it, as Linux's PIT has a host timer of its own. */
static int wake_cond(void *arg)
{
    struct kvm_vcpu *v = (struct kvm_vcpu *)arg;
    struct lapic *l = apic_of(v);
    uint64_t next;

    if ((v->requests & (KVM_REQ_EXIT | KVM_REQ_TRIPLE_FAULT | KVM_REQ_TIMER)) ||
        kvm_irq_pending(v->vm)) {
        return 1;
    }
    if (l->events & EV_INIT) {
        return 1;
    }
    if (v->mp_state == KVM_MP_STATE_INIT_RECEIVED || v->mp_state == KVM_MP_STATE_UNINITIALIZED) {
        return (l->events & (EV_SIPI | EV_SIPI_SET)) != 0;
    }
    if (l->nmis || v->nmi_queued || l->pv_unhalted) {
        return 1;
    }
    if (guest_if(v) && kvm_lapic_pending(v) >= 0) {
        return 1;
    }
    next = next_ns(l);
    if (next && kvm_host_ns() >= next) {
        return 1;
    }
    return l->wake_tick && tick_count() >= l->wake_tick;
}

/** Convert a raw guest-timer expiry to this wait's monotonic deadline.
 * Pair raw reads around the monotonic sample so a host stop between reads
 * cannot add its whole duration to the wait. No spinlock or big lock is
 * held; a deadline beyond signed nanoseconds keeps its coarse fallback. */
int64_t kvm_timer_deadline(uint64_t next)
{
    uint64_t before, after, gap, best = ~(uint64_t)0, middle = 0, span;
    int64_t mono = 0, sample;
    int i;

    if (!next) {
        return 0;
    }
    for (i = 0; i < 16 && (i == 0 || best > 50000u); i++) {
        before = kvm_host_ns();
        sample = rtc_monotonic_ns();
        after = kvm_host_ns();
        gap = after - before;
        if (gap < best) {
            best = gap;
            middle = before + gap / 2u;
            mono = sample;
        }
    }
    span = next > middle ? rtc_slewed(next - middle, rtc_mono_rate()) : 1u;
    if (span > (uint64_t)0x7FFFFFFFFFFFFFFFull - (uint64_t)mono) {
        return 0;
    }
    return mono + (int64_t)span;
}

/** Spin for up to `kvm_halt_poll_ns` waiting for the halted vCPU `v` to
 * have an event, as Linux's kvm_vcpu_halt polls before it sleeps
 * (virt/kvm/kvm_main.c:3822-3858): a wake that comes within the window
 * costs no sleep and no switch back. The spin ends early when the thread
 * has work, a signal or a switch the scheduler asked for, so that a thread
 * that needs the processor gets it. 1 when an event came in the window,
 * counted in halt_successful_poll and halt_poll_success_ns; 0 when the
 * window passed or was cut short, counted in halt_poll_fail_ns. Called
 * with no lock held; does not sleep. */
static int poll(struct kvm_vcpu *v)
{
    uint64_t start, now;
    int32_t window = kvm_halt_poll_ns;

    if (window <= 0 || !current_proc) {
        return 0;
    }
    v->stat[KVM_VCPU_STAT_HALT_ATTEMPTED_POLL]++;
    start = kvm_host_ns();
    now = start;
    while (now - start < (uint64_t)window) {
        if (wake_cond(v) || (v->requests & KVM_REQ_IRQFD)) {
            v->stat[KVM_VCPU_STAT_HALT_SUCCESSFUL_POLL]++;
            v->stat[KVM_VCPU_STAT_HALT_POLL_SUCCESS_NS] += kvm_host_ns() - start;
            return 1;
        }
        if (current_proc->work || current_proc->need_resched) {
            break;
        }
        arch_relax();
        now = kvm_host_ns();
    }
    v->stat[KVM_VCPU_STAT_HALT_POLL_FAIL_NS] += kvm_host_ns() - start;
    return 0;
}

/** Wait, interruptibly, until the vCPU `v` has an event or its timer's
 * fine deadline or fallback tick comes: 0, or -EINTR when the thread's
 * signal-work flag is set. The
 * caller holds no big lock, so the wait reads the flag alone, as Linux's
 * kvm_vcpu_check_block (virt/kvm/kvm_main.c:3729-3747). On -EINTR the
 * caller ends KVM_RUN with KVM_EXIT_INTR, as Linux's vcpu_run takes the
 * signal after vcpu_block in kvm_handle_signal_exit
 * (include/linux/kvm_host.h:2405-2409, kernel/entry/kvm.c:11-13), and
 * leaves the signal to the system call's return. */
static int32_t block(struct kvm_vcpu *v)
{
    struct lapic *l = apic_of(v);
    uint64_t next, now;
    int32_t r;

    if (poll(v)) {
        return 0;
    }
    next = next_ns(l);
    l->wake_tick = 0;
    if (next) {
        now = kvm_host_ns();
        l->wake_tick = tick_count() + (next > now ? (next - now + TICK_NS - 1u) / TICK_NS : 0) + 1u;
    }
    proc_wait_set(l->wake_tick, kvm_timer_deadline(next), 0, 0);
    r = wq_wait_event_sigflag(&l->wq, wake_cond, v, "kvm_vcpu_block");
    proc_wait_set(0, 0, 0, 0);
    l->wake_tick = 0;
    return r;
}

int32_t kvm_lapic_prepare(struct kvm_vcpu *v)
{
    struct lapic *l = apic_of(v);
    int32_t r;

    for (;;) {
        if (kvm_vcpu_test_clear(v, KVM_REQ_IRQFD)) {
            kvm_irq_process(v->vm);
        }
        if (kvm_vcpu_test_clear(v, KVM_REQ_NMI)) {
            take_nmis(l);
        }
        if (kvm_vcpu_test_clear(v, KVM_REQ_INIT_SIPI)) {
            accept_events(l);
        }
        if (kvm_vcpu_test_clear(v, KVM_REQ_SCAN_IOAPIC)) {
            kvm_irq_scan_eoi_exits(v, l->eoi_exit); /* Linux's vcpu_scan_ioapic */
        }
        if (kvm_vcpu_test_clear(v, KVM_REQ_EOI_EXIT)) {
            v->run->exit_reason = KVM_EXIT_IOAPIC_EOI;
            v->run->u.eoi.vector = (uint8_t)l->eoi_vector;
            return 1;
        }
        if (v->requests & (KVM_REQ_EXIT | KVM_REQ_TRIPLE_FAULT)) {
            return 0;
        }
        if (v->mp_state == KVM_MP_STATE_RUNNABLE) {
            break;
        }
        if ((v->mp_state == KVM_MP_STATE_HALTED || v->mp_state == KVM_MP_STATE_AP_RESET_HOLD) &&
            (v->nmi_queued || l->pv_unhalted || (guest_if(v) && kvm_lapic_pending(v) >= 0))) {
            l->pv_unhalted = 0;
            v->mp_state = KVM_MP_STATE_RUNNABLE;
            v->dirty |= KVM_PART_MP;
            /* The event that woke it goes in at this entry. */
            kvm_vcpu_kick(v, KVM_REQ_EVENT);
            break;
        }
        if (kvm_dirty_ring_full(v)) {
            /* The synthetic timers' writes filled the ring; the vCPU stays
             * halted until the caller resets it and runs again. */
            v->run->exit_reason = KVM_EXIT_DIRTY_RING_FULL;
            return 1;
        }
        r = block(v);
        if (r != 0) {
            v->run->exit_reason = KVM_EXIT_INTR;
            v->stat[KVM_VCPU_STAT_SIGNAL_EXITS]++;
            return -EINTR;
        }
        /* A wake for a timer finds it due here, whatever `timer_ns` says. */
        kvm_vcpu_kick(v, KVM_REQ_TIMER);
        kvm_vcpu_timers(v);
    }
    return 0;
}

/** KVM_GET_LAPIC: the page, with LVR and TMCCT as the guest reads them,
 * and in x2APIC mode the id in xAPIC's place unless
 * KVM_X2APIC_API_USE_32BIT_IDS asked for all 32 bits, as Linux's
 * kvm_apic_state_fixup gives it. */
static int32_t get_lapic(struct lapic *l, uintptr_t arg)
{
    struct kvm_lapic_state s;
    uint32_t i, w;

    for (i = 0; i < KVM_APIC_REG_SIZE / 4; i++) {
        w = l->regs[i];
        if (i * 4u == APIC_LVR) {
            w = lvr_of(l->v);
        } else if (i * 4u == APIC_TMCCT) {
            w = tmcct(l);
        } else if (i * 4u == APIC_ID && x2apic(l) && !l->v->vm->x2apic_format) {
            w <<= 24;
        }
        s.regs[i * 4u] = (uint8_t)w;
        s.regs[i * 4u + 1u] = (uint8_t)(w >> 8);
        s.regs[i * 4u + 2u] = (uint8_t)(w >> 16);
        s.regs[i * 4u + 3u] = (uint8_t)(w >> 24);
    }
    return copy_to_user((void __user *)arg, &s, sizeof(s)) ? -EFAULT : 0;
}

/** KVM_SET_LAPIC, as Linux's kvm_apic_set_state: the page, with PPR and
 * the timer's mode and divisor applied, and the count restarted from the
 * TMCCT given. In x2APIC mode the id is the vCPU's, which the caller
 * cannot change, and the LDR follows from it; with
 * KVM_X2APIC_API_USE_32BIT_IDS an id other than the vCPU's is -EINVAL,
 * as in Linux's kvm_apic_state_fixup. -EFAULT for a bad pointer. */
static int32_t set_lapic(struct lapic *l, uintptr_t arg)
{
    struct kvm_lapic_state s;
    const uint8_t *p;
    uint32_t i;

    if (copy_from_user(&s, (const void __user *)arg, sizeof(s)) != 0) {
        return -EFAULT;
    }
    p = s.regs + APIC_ID;
    if (x2apic(l) && l->v->vm->x2apic_format &&
        ((uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24) !=
            l->v->id) {
        return -EINVAL;
    }
    for (i = 0; i < KVM_APIC_REG_SIZE / 4; i++) {
        p = s.regs + i * 4u;
        l->regs[i] =
            (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
    }
    if (x2apic(l)) {
        reg_set(l, APIC_ID, l->v->id);
        reg_set(l, APIC_LDR, x2apic_ldr(l->v->id));
    }
    reg_set(l, APIC_LVR, lvr_of(l->v));
    update_ppr(l);
    timer_cancel(l);
    update_lvtt(l);
    update_divide(l);
    if (l->timer_mode != TIMER_DEADLINE) {
        timer_start(l, APIC_TMCCT);
    }
    reg_set(l, APIC_TMCCT, 0);
    kvm_ioapic_restore(l->v);
    dest_changed(l);
    kvm_vcpu_kick(l->v, KVM_REQ_EVENT | KVM_REQ_TIMER);
    return 0;
}

/** KVM_SET_MP_STATE with the APIC in the kernel, as Linux's
 * kvm_arch_vcpu_ioctl_set_mpstate: SIPI_RECEIVED waits for SIPI with one
 * pending, at the vector KVM_SET_VCPU_EVENTS gave, before or after. */
static int32_t set_mp_state(struct lapic *l, uintptr_t arg)
{
    struct kvm_mp_state mp;
    struct kvm_vcpu *v = l->v;

    if (copy_from_user(&mp, (const void __user *)arg, sizeof(mp)) != 0) {
        return -EFAULT;
    }
    switch (mp.mp_state) {
    case KVM_MP_STATE_RUNNABLE:
    case KVM_MP_STATE_UNINITIALIZED:
    case KVM_MP_STATE_INIT_RECEIVED:
    case KVM_MP_STATE_HALTED:
    case KVM_MP_STATE_AP_RESET_HOLD:
        v->mp_state = mp.mp_state;
        break;
    case KVM_MP_STATE_SIPI_RECEIVED:
        v->mp_state = KVM_MP_STATE_INIT_RECEIVED;
        atomic_or(&l->events, EV_SIPI_SET);
        break;
    default:
        return -EINVAL;
    }
    v->dirty |= KVM_PART_MP;
    kvm_vcpu_kick(v, KVM_REQ_INIT_SIPI | KVM_REQ_EVENT);
    return 0;
}

int32_t kvm_lapic_ioctl(struct kvm_vcpu *v, uint32_t request, uintptr_t arg)
{
    struct lapic *l = apic_of(v);
    struct kvm_interrupt irq;
    struct kvm_mp_state mp;

    if (!l) {
        return -EINVAL;
    }
    switch (request) {
    case KVM_GET_LAPIC:
        return get_lapic(l, arg);
    case KVM_SET_LAPIC:
        return set_lapic(l, arg);
    case KVM_INTERRUPT:
        /* With the APIC in the kernel, KVM_INTERRUPT gives the caller's
         * PIC's vector, one at a time, as Linux's
         * kvm_vcpu_ioctl_interrupt. */
        if (copy_from_user(&irq, (const void __user *)arg, sizeof(irq)) != 0) {
            return -EFAULT;
        }
        if (v->vm->irqchip == KVM_IRQCHIP_KERNEL) {
            return -ENXIO;
        }
        if (irq.irq >= KVM_NR_INTERRUPTS) {
            return -EINVAL;
        }
        if (l->ext_vector >= 0) {
            return -EEXIST;
        }
        l->ext_vector = (int)irq.irq;
        kvm_vcpu_kick(v, KVM_REQ_EVENT);
        return 0;
    case KVM_GET_MP_STATE:
        accept_events(l);
        mp.mp_state = v->mp_state;
        if ((mp.mp_state == KVM_MP_STATE_HALTED || mp.mp_state == KVM_MP_STATE_AP_RESET_HOLD) &&
            l->pv_unhalted) {
            mp.mp_state = KVM_MP_STATE_RUNNABLE;
        }
        return copy_to_user((void __user *)arg, &mp, sizeof(mp)) ? -EFAULT : 0;
    case KVM_SET_MP_STATE:
        return set_mp_state(l, arg);
    default:
        return -EINVAL;
    }
}
