/*
 * lapic.c: the local APIC, for message-signalled interrupts, the I/O APIC's
 * interrupts and the tick.
 *
 * A message from a PCI device is a write to the APIC's address, so the APIC
 * must be enabled before any device is told to send one. It is enabled the
 * same way whichever controller then takes the machine's lines: with the
 * 8259 pair, whose interrupts reach the processor through `LINT0` as an
 * external interrupt, the state firmware calls virtual wire; or with the I/O
 * APIC, whose interrupts arrive as messages, after which the platform masks
 * `LINT0` and the 8259 with it (platform.c).
 *
 * Enabling the APIC with `LINT0` masked would silence the 8259's timer and
 * keyboard, so this driver reads that entry after the APIC is enabled and
 * writes the unmasked external-interrupt value when it finds anything else.
 * `LINT1` is left as the firmware set it until the platform writes the
 * entry the MADT gives (`apic_lint_init`).
 *
 * The APIC's own timer counts down from an initial count at the bus clock
 * divided by sixteen, and in periodic mode reloads and raises its vector
 * each time it reaches zero; it is the tick when the I/O APIC takes the
 * lines (apictimer.c). Every register here is the running processor's own.
 *
 * The APIC has two modes. In xAPIC mode its registers are a page of memory;
 * in x2APIC mode they are model-specific registers from 0x800, one for each
 * 16 bytes of the page, and the interrupt command register is one 64-bit
 * register whose single write sends the message, with no delivery status to
 * wait on (SDM, the x2APIC's register address space). Under a hypervisor
 * each access to the page is an exit with its instruction decoded, and an
 * xAPIC interprocessor interrupt is three of them, so x2APIC mode is used
 * where Linux 6.12.115 uses it: under a hypervisor that delivers interrupts
 * to it unremapped (`kcpu_x2apic_unremapped`), from `lapic_init`; and on
 * bare metal once the VT-d units remap interrupts in x2APIC format
 * (`lapic_x2apic_enable`, iommu/intremap.c), since an x2APIC without
 * remapping is not supported there. That is Linux's try_to_enable_x2apic
 * and check_x2apic (arch/x86/kernel/apic/apic.c, lines 1820 to 1873). A
 * firmware that left the APIC in x2APIC mode on bare metal has it returned
 * to xAPIC mode, as Linux's x2apic_disable does, unless it locked it there.
 * Destinations are physical, as x2apic_phys.c gives them: every message
 * names one processor. `nox2apic` keeps xAPIC mode, as on Linux.
 *
 * A processor without an APIC or without the model-specific registers, in
 * an x2APIC mode it cannot leave with an identity above 254, or in xAPIC
 * mode with its registers above 4 GiB, at no address or on a page that
 * cannot be mapped, is refused by name: the kernel then has no message
 * vectors and every device stays on its legacy line. `rach2.nomsi` refuses
 * in the same way, so a machine that misbehaves has one word on its boot
 * line.
 */

#include "lapic.h"

#include "../../cmdline.h"
#include "../../klog.h"
#include "../../lib/kirq.h"
#include "kcpu.h"
#include "../../mm/paging.h"
#include "../../proc/percpu.h"

#define IA32_APIC_BASE   0x1B
#define APIC_BASE_ENABLE (1u << 11)
#define APIC_BASE_X2APIC (1u << 10)

#define LAPIC_ID      0x20
#define LAPIC_VERSION 0x30
#define LAPIC_TPR     0x80
#define LAPIC_EOI     0xB0
#define LAPIC_SVR     0xF0
#define LAPIC_ISR     0x100
#define LAPIC_IRR     0x200
#define LAPIC_ICR_LO  0x300
#define LAPIC_ICR_HI  0x310
#define LAPIC_TIMER   0x320
#define LAPIC_LVTPC   0x340
#define LAPIC_LINT0   0x350
#define LAPIC_LINT1   0x360
#define LAPIC_INITIAL 0x380
#define LAPIC_CURRENT 0x390
#define LAPIC_DIVIDE  0x3E0
#define LAPIC_SELF    0x3F0 /* x2APIC mode's self-interrupt register */
#define LAPIC_ERROR   0x370

/* The x2APIC's registers: the xAPIC offset over 16, from 0x800. */
#define X2APIC_MSR(reg) (0x800u + (reg) / 16u)

#define LVT_MASKED          (1u << 16)
#define LVT_DELIVERY_MASK   (7u << 8)
#define LVT_DELIVERY_EXTINT (7u << 8)
#define SVR_SOFTWARE_ENABLE 0x100u
#define SVR_EOI_SUPPRESS    0x1000u
#define SPURIOUS_VECTOR     0xFFu
#define TIMER_PERIODIC      (1u << 17)
#define DIVIDE_BY_16        0x3u
#define ICR_SELF            (1u << 18)
#define ICR_PENDING         (1u << 12)
#define ICR_NMI             (4u << 8)
#define ICR_INIT            (5u << 8)
#define ICR_STARTUP         (6u << 8)
#define ICR_ASSERT          (1u << 14)
#define ICR_LEVEL           (1u << 15)

/* The APIC's register page as `lapic_init` mapped it, NULL until then, in
 * x2APIC mode, and while a host test's `io` stands in for it; the host
 * test's access, or NULL; the boot processor's APIC identity; whether
 * `lapic_init` enabled the APIC, which every access but its own checks;
 * and whether every processor's APIC is in x2APIC mode, its registers
 * MSRs, which changes only on the boot processor before the others start. */
static volatile uint32_t *apic;
static const struct lapic_io *io;
static uint32_t apic_id;
static int ready;
static int x2apic;

/* The spurious vectors each processor took, in a word of its own, since the
 * handler runs without a lock on every processor. */
static uint32_t spurious_count[NR_CPUS];

void lapic_set_io(const struct lapic_io *ops)
{
    io = ops;
}

/** Read the APIC register at offset `reg`: its MSR in x2APIC mode, else
 * through `io` when a host test gave one, else the page. */
static uint32_t apic_read(uint32_t reg)
{
    if (x2apic) {
        uint32_t lo = 0;

        kcpu_rdmsr(X2APIC_MSR(reg), &lo, 0);
        return lo;
    }
    if (io) {
        return io->read(reg);
    }
    return apic[reg / 4];
}

/** Write the APIC register at offset `reg`: its MSR in x2APIC mode, else
 * through `io` when a host test gave one, else the page. */
static void apic_write(uint32_t reg, uint32_t value)
{
    if (x2apic) {
        kcpu_wrmsr(X2APIC_MSR(reg), value, 0);
        return;
    }
    if (io) {
        io->write(reg, value);
        return;
    }
    apic[reg / 4] = value;
    (void)apic[LAPIC_ID / 4]; /* a read the write cannot pass */
}

/** The running processor's APIC identity: the whole x2APIC identity, or
 * bits 31:24 of the xAPIC's register. */
static uint32_t lapic_this_id_raw(void)
{
    return x2apic ? apic_read(LAPIC_ID) : apic_read(LAPIC_ID) >> 24;
}

/** Log why `lapic_init` refused, and return -1. */
static int refuse(const char *why)
{
    klog(KLOG_WARN, "lapic", "refused: %s", why);
    return -1;
}

/**
 * The spurious register `svr` with the software enable and the spurious
 * vector, and with whatever else the firmware set there kept but bit 12. A
 * firmware that set it suppresses the end-of-interrupt the local APIC
 * broadcasts to the I/O APICs, which clears a level-triggered line's remote
 * IRR; the kernel sends no directed one, so the line's first interrupt
 * would be its last.
 */
static uint32_t lapic_svr(uint32_t svr)
{
    return (svr & ~(0xFFu | SVR_EOI_SUPPRESS)) | SVR_SOFTWARE_ENABLE | SPURIOUS_VECTOR;
}

/** Whether x2APIC mode may be used: the processor has it and the boot line
 * has no `nox2apic`, Linux's word that clears the processor's x2APIC bit. */
static int x2apic_offered(void)
{
    return !kcmdline_has("nox2apic") && kcpu_x2apic_present();
}

/** Return this processor's APIC from x2APIC to xAPIC mode through the
 * disabled state, since the SDM allows no direct step, as Linux's
 * __x2apic_disable writes it (apic.c, lines 1706 to 1720). `lo` and `hi`
 * are IA32_APIC_BASE as read. */
static void x2apic_leave(uint32_t lo, uint32_t hi)
{
    kcpu_wrmsr(IA32_APIC_BASE, lo & ~(APIC_BASE_X2APIC | APIC_BASE_ENABLE), hi);
    kcpu_wrmsr(IA32_APIC_BASE, (lo & ~APIC_BASE_X2APIC) | APIC_BASE_ENABLE, hi);
}

/**
 * Choose the boot processor's mode from IA32_APIC_BASE's `*lo` and `hi`,
 * as check_x2apic and try_to_enable_x2apic do for a kernel with no
 * interrupt remapping yet: x2APIC mode (`x2apic` set) under a hypervisor
 * that allows it, or where the firmware locked it; otherwise xAPIC mode,
 * with a firmware's x2APIC mode left (`*lo` updated). 0, or -1 when the
 * firmware's x2APIC identity is past what xAPIC mode can address.
 */
static int choose_mode(uint32_t *lo, uint32_t hi)
{
    uint32_t id = 0;

    x2apic = x2apic_offered() && kcpu_x2apic_unremapped();
    if ((*lo & APIC_BASE_X2APIC) == 0 || x2apic) {
        return 0;
    }
    if (kcpu_x2apic_locked()) {
        x2apic = 1;
        klog(KLOG_WARN, "lapic", "x2apic: locked on by the firmware; kept");
        return 0;
    }
    kcpu_rdmsr(X2APIC_MSR(LAPIC_ID), &id, 0);
    if (id >= 0xFFu) {
        return -1;
    }
    x2apic_leave(*lo, hi);
    *lo = (*lo & ~APIC_BASE_X2APIC) | APIC_BASE_ENABLE;
    klog(KLOG_INFO, "lapic", "x2apic: left on by the firmware; returned to xapic mode");
    return 0;
}

/**
 * In x2APIC mode no page is mapped and the base is not read.
 *
 * In xAPIC mode a base whose upper half is not zero is above 4 GiB, where
 * this kernel's addresses do not reach, and taking its low half would map
 * another device's page.
 *
 * The software enable comes first, and the local-vector entries after it.
 * While the spurious register's bit 8 is clear the processor holds the mask
 * bit of every entry set and ignores a write that would clear one (SDM, the
 * local APIC's state after it has been software disabled), so an unmasked
 * `LINT0` written before the enable would be dropped and the 8259's
 * interrupts would stop at the processor once the APIC came up. The
 * spurious register is written as read, with the vector and the enable
 * (`lapic_svr`).
 */
int lapic_init(void)
{
    uint32_t lo = 0, hi = 0, base, lint0;
    uintptr_t page;

    ready = 0;
    x2apic = 0;
    if (kcmdline_has("rach2.nomsi")) {
        return refuse("rach2.nomsi on the boot line");
    }
    if (!kcpu_apic_present()) {
        return refuse("the processor reports no local apic");
    }
    if (!kcpu_msr_present()) {
        return refuse("the processor has no model-specific registers");
    }
    kcpu_rdmsr(IA32_APIC_BASE, &lo, &hi);
    if (choose_mode(&lo, hi) != 0) {
        return refuse("x2apic mode with an identity past 254, which xapic mode cannot address");
    }
    base = lo & 0xFFFFF000u;
    if (!x2apic) {
        if (hi != 0) {
            return refuse("an apic base above 4 gib");
        }
        if (base == 0) {
            return refuse("no apic base address");
        }
        if (!io) {
            page = map_mmio((uintptr_t)base, 0x1000);
            if (page == 0) {
                return refuse("the apic page could not be mapped");
            }
            apic = (volatile uint32_t *)page;
        }
    }
    if ((lo & APIC_BASE_ENABLE) == 0) {
        lo |= APIC_BASE_ENABLE;
        kcpu_wrmsr(IA32_APIC_BASE, lo, hi);
    }
    if (x2apic && (lo & APIC_BASE_X2APIC) == 0) {
        kcpu_wrmsr(IA32_APIC_BASE, lo | APIC_BASE_X2APIC, hi);
    }
    apic_id = lapic_this_id_raw();

    apic_write(LAPIC_SVR, lapic_svr(apic_read(LAPIC_SVR)));
    apic_write(LAPIC_TPR, 0);
    lint0 = apic_read(LAPIC_LINT0);
    if ((lint0 & LVT_MASKED) != 0 || (lint0 & LVT_DELIVERY_MASK) != LVT_DELIVERY_EXTINT) {
        /* Without this the 8259's interrupts stop at the processor. */
        apic_write(LAPIC_LINT0, LVT_DELIVERY_EXTINT);
        klog(KLOG_WARN, "lapic", "lint0 was 0x%x; set to an unmasked external interrupt", lint0);
    }
    ready = 1;
    klog(KLOG_DEBUG, "lapic", "ready: id %d, version 0x%x, %s", (int)apic_id,
         apic_read(LAPIC_VERSION) & 0xFFu, x2apic ? "x2apic mode" : "xapic mode");
    return 0;
}

/* The registers a change of mode must keep: the spurious register and task
 * priority, the local-vector entries this kernel writes, and the timer's
 * divisor and count. */
static const uint32_t kept_regs[] = {LAPIC_SVR,   LAPIC_TPR,    LAPIC_TIMER,
                                     LAPIC_LINT0, LAPIC_LINT1,  LAPIC_LVTPC,
                                     LAPIC_ERROR, LAPIC_DIVIDE, LAPIC_INITIAL};
#define NKEPT ((int)(sizeof(kept_regs) / sizeof(kept_regs[0])))

/**
 * Move the boot processor's APIC to x2APIC mode (`to` 1) or back (`to` 0),
 * with interrupts off, and write again each register in `kept_regs` that
 * the move changed. The SDM keeps every register but the identity and the
 * logical destination from xAPIC to x2APIC mode, and resets them all on the
 * way back through the disabled state; the count of registers written
 * again is logged, so that a processor that kept less shows it.
 */
static void switch_mode(int to)
{
    uint32_t saved[NKEPT], lo = 0, hi = 0, flags;
    int i, rewritten = 0;

    flags = kirq_save();
    for (i = 0; i < NKEPT; i++) {
        saved[i] = apic_read(kept_regs[i]);
    }
    kcpu_rdmsr(IA32_APIC_BASE, &lo, &hi);
    if (to) {
        kcpu_wrmsr(IA32_APIC_BASE, lo | APIC_BASE_X2APIC, hi);
    } else {
        x2apic_leave(lo, hi);
    }
    x2apic = to;
    for (i = 0; i < NKEPT; i++) {
        if (apic_read(kept_regs[i]) != saved[i]) {
            apic_write(kept_regs[i], saved[i]);
            rewritten++;
        }
    }
    apic_id = lapic_this_id_raw();
    kirq_restore(flags);
    klog(rewritten ? KLOG_WARN : KLOG_INFO, "lapic", "%s: id %d, %d register(s) written again",
         to ? "x2apic mode" : "xapic mode", (int)apic_id, rewritten);
}

int lapic_x2apic(void)
{
    return ready && x2apic;
}

int lapic_x2apic_possible(void)
{
    return ready && (x2apic || x2apic_offered());
}

/** A page must be mapped to come back, which `lapic_init` mapped in xAPIC
 * mode. */
int lapic_x2apic_enable(void)
{
    if (!lapic_x2apic_possible()) {
        return -1;
    }
    if (x2apic) {
        return 0;
    }
    switch_mode(1);
    return 1;
}

void lapic_x2apic_disable(void)
{
    if (ready && x2apic && (apic != NULL || io != NULL) && !kcpu_x2apic_unremapped() &&
        !kcpu_x2apic_locked()) {
        switch_mode(0);
    }
}

/** The request and in-service registers have 256 bits each, 32 to a word,
 * the words 16 bytes apart. */
int lapic_pending(uint8_t vector)
{
    uint32_t word = ((uint32_t)vector / 32u) * 0x10u;
    uint32_t bit = 1u << ((uint32_t)vector % 32u);

    if (!ready) {
        return 0;
    }
    return (apic_read(LAPIC_IRR + word) & bit) != 0 || (apic_read(LAPIC_ISR + word) & bit) != 0;
}

int lapic_ready(void)
{
    return ready;
}

void lapic_repair_lint0(void)
{
    if (!ready) {
        return;
    }
    apic_write(LAPIC_LINT0, LVT_DELIVERY_EXTINT);
}

uint32_t lapic_id(void)
{
    return apic_id;
}

uint32_t lapic_this_id(void)
{
    return ready ? lapic_this_id_raw() : 0;
}

void lapic_eoi(void)
{
    if (ready) {
        apic_write(LAPIC_EOI, 0);
    }
}

/** The APIC raises vector 255 when an interrupt it was about to deliver
 * went away, which needs no end-of-interrupt and no work. */
void lapic_spurious(void)
{
    spurious_count[arch_cpu_id()]++;
}

uint32_t lapic_spurious_count(void)
{
    uint32_t sum = 0;
    int i;

    for (i = 0; i < NR_CPUS; i++) {
        sum += spurious_count[i];
    }
    return sum;
}

int lapic_requested(uint8_t vector)
{
    uint32_t word = ((uint32_t)vector / 32u) * 0x10u;

    if (!ready) {
        return 0;
    }
    return (apic_read(LAPIC_IRR + word) & (1u << ((uint32_t)vector % 32u))) != 0;
}

void lapic_set_lint(int lint, uint32_t entry)
{
    if (ready) {
        apic_write(lint == 0 ? LAPIC_LINT0 : LAPIC_LINT1, entry);
    }
}

/** The self shorthand ignores the destination, so the two writes need not
 * be kept together as `send_command` keeps them. x2APIC mode has a register
 * for it, which Linux's x2apic_send_IPI_self writes (x2apic_phys.c, lines
 * 101 to 104). */
void lapic_self_ipi(uint8_t vector)
{
    int spins;

    if (!ready) {
        return;
    }
    if (x2apic) {
        apic_write(LAPIC_SELF, vector);
        return;
    }
    apic_write(LAPIC_ICR_HI, 0);
    apic_write(LAPIC_ICR_LO, ICR_SELF | vector);
    for (spins = 0; spins < 100000 && (apic_read(LAPIC_ICR_LO) & ICR_PENDING) != 0; spins++) {
        /* the delivery status clears once the message is sent */
    }
}

/** The processor's mode is made the boot processor's first, as Linux's
 * x2apic_setup makes it (apic.c, lines 1757 to 1777). */
void lapic_init_ap(void)
{
    uint32_t lo = 0, hi = 0;

    if (!ready) {
        return;
    }
    kcpu_rdmsr(IA32_APIC_BASE, &lo, &hi);
    if (x2apic && (lo & APIC_BASE_X2APIC) == 0) {
        kcpu_wrmsr(IA32_APIC_BASE, lo | APIC_BASE_ENABLE, hi);
        kcpu_wrmsr(IA32_APIC_BASE, lo | APIC_BASE_ENABLE | APIC_BASE_X2APIC, hi);
    } else if (!x2apic && (lo & APIC_BASE_X2APIC) != 0) {
        x2apic_leave(lo, hi);
    }
    apic_write(LAPIC_SVR, lapic_svr(apic_read(LAPIC_SVR)));
    apic_write(LAPIC_TPR, 0);
}

/**
 * Write the interrupt command register for the processor `target`.
 *
 * In xAPIC mode the destination goes in the high word first, since the
 * write of the low word sends the message, and the APIC is waited on until
 * it has sent it. Interrupts are off across the two writes: a handler that
 * sent its own message between them would leave this one with the other's
 * destination.
 *
 * In x2APIC mode the command is one write of the 64-bit register, the
 * destination in its high half, Linux's native_x2apic_icr_write
 * (arch/x86/include/asm/apic.h, lines 231 to 234), with nothing to wait on.
 * A write of an x2APIC register does not serialize, so MFENCE and LFENCE
 * come first, Linux's weak_wrmsr_fence (asm/processor.h, lines 760 to
 * 763, which x2apic_send_IPI calls at x2apic_phys.c line 48): a store made
 * before the message must reach the processor it wakes.
 */
static void send_command(uint32_t target, uint32_t low)
{
    uint32_t flags;
    int spins;

    if (!ready) {
        return;
    }
    if (x2apic) {
        /* mfence and lfence as bytes: i686 tcc has no mnemonic for them. */
        __asm__ __volatile__(".byte 0x0f, 0xae, 0xf0\n\t.byte 0x0f, 0xae, 0xe8" : : : "memory");
        kcpu_wrmsr(X2APIC_MSR(LAPIC_ICR_LO), low, target);
        return;
    }
    flags = kirq_save();
    apic_write(LAPIC_ICR_HI, target << 24);
    apic_write(LAPIC_ICR_LO, low);
    for (spins = 0; spins < 100000 && (apic_read(LAPIC_ICR_LO) & ICR_PENDING) != 0; spins++) {
        /* the delivery status clears once the message is sent */
    }
    kirq_restore(flags);
}

void lapic_send_ipi(uint32_t target, uint8_t vector)
{
    send_command(target, vector);
}

void lapic_send_nmi(uint32_t target)
{
    send_command(target, ICR_NMI);
}

void lapic_set_lvtpc(uint32_t entry)
{
    if (ready) {
        apic_write(LAPIC_LVTPC, entry);
    }
}

void lapic_send_init(uint32_t target)
{
    send_command(target, ICR_INIT | ICR_LEVEL | ICR_ASSERT);
}

void lapic_send_startup(uint32_t target, uint32_t page)
{
    send_command(target, ICR_STARTUP | (page & 0xFFu));
}

/** The divide register is written before the initial count, which starts
 * the count. */
void lapic_timer_set(uint8_t vector, int periodic, int masked, uint32_t initial)
{
    if (!ready) {
        return;
    }
    apic_write(LAPIC_DIVIDE, DIVIDE_BY_16);
    apic_write(LAPIC_TIMER, vector | (periodic ? TIMER_PERIODIC : 0u) | (masked ? LVT_MASKED : 0u));
    apic_write(LAPIC_INITIAL, initial);
}

void lapic_timer_stop(void)
{
    if (!ready) {
        return;
    }
    apic_write(LAPIC_INITIAL, 0);
    apic_write(LAPIC_TIMER, LVT_MASKED);
}

uint32_t lapic_timer_current(void)
{
    return ready ? apic_read(LAPIC_CURRENT) : 0;
}

uint32_t lapic_timer_initial(void)
{
    return ready ? apic_read(LAPIC_INITIAL) : 0;
}
