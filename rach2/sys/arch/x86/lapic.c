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
 * `LINT1` is left as the firmware set it until the platform writes the NMI
 * entry the MADT gives.
 *
 * The APIC's own timer counts down from an initial count at the bus clock
 * divided by sixteen, and in periodic mode reloads and raises its vector
 * each time it reaches zero; it is the tick when the I/O APIC takes the
 * lines (apictimer.c). Every register here is the running processor's own.
 *
 * A processor without an APIC, without the model-specific registers, in
 * x2APIC mode, or with its registers somewhere other than the page this
 * driver maps, is refused by name: the kernel then has no message vectors
 * and every device stays on its legacy line. `rach2.nomsi` refuses in the
 * same way, so a machine that misbehaves has one word on its boot line.
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
#define LAPIC_LINT0   0x350
#define LAPIC_LINT1   0x360
#define LAPIC_INITIAL 0x380
#define LAPIC_CURRENT 0x390
#define LAPIC_DIVIDE  0x3E0

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
#define ICR_INIT            (5u << 8)
#define ICR_STARTUP         (6u << 8)
#define ICR_ASSERT          (1u << 14)
#define ICR_LEVEL           (1u << 15)

/* The page the APIC's registers live on, and zero until `lapic_init` maps
 * it, which is what `lapic_ready` reports and every write here checks. */
static volatile uint32_t *apic;
static const struct lapic_io *io;
static uint32_t apic_id;
static int ready;

/* The spurious vectors each processor took, in a word of its own, since the
 * handler runs without a lock on every processor. */
static uint32_t spurious_count[NR_CPUS];

void lapic_set_io(const struct lapic_io *ops)
{
    io = ops;
}

static uint32_t apic_read(uint32_t reg)
{
    if (io) {
        return io->read(reg);
    }
    return apic[reg / 4];
}

static void apic_write(uint32_t reg, uint32_t value)
{
    if (io) {
        io->write(reg, value);
        return;
    }
    apic[reg / 4] = value;
    (void)apic[LAPIC_ID / 4]; /* a read the write cannot pass */
}

static int refuse(const char *why)
{
    klog(KLOG_WARN, "lapic", "refused: %s", why);
    return -1;
}

/**
 * Enable the local APIC, or refuse with the reason named.
 *
 * The order matters, and it is the opposite of what it looks like it should
 * be: the software enable goes first, because a processor whose APIC is
 * software disabled holds every local-vector entry masked and ignores a write
 * that would unmask one. The task priority and `LINT0` are written after it.
 * 0 when the APIC is ready to receive messages, -1 when it is not.
 */
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

int lapic_init(void)
{
    uint32_t lo = 0, hi = 0, base, lint0;
    uintptr_t page;

    ready = 0;
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
    if ((lo & APIC_BASE_X2APIC) != 0) {
        return refuse("x2apic mode, whose registers this driver does not read");
    }
    if (hi != 0) {
        /* The upper half of the base is not zero, so the registers are above
         * 4 GiB and this kernel's addresses do not reach them. Taking the low
         * half would map some other device's page. */
        return refuse("an apic base above 4 gib");
    }
    base = lo & 0xFFFFF000u;
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
    if ((lo & APIC_BASE_ENABLE) == 0) {
        kcpu_wrmsr(IA32_APIC_BASE, lo | APIC_BASE_ENABLE, hi);
    }
    apic_id = apic_read(LAPIC_ID) >> 24;

    /*
     * The software enable comes first, and the local-vector entries after it.
     * While the spurious register's bit 8 is clear the processor holds the
     * mask bit of every entry set and ignores a write that would reset one
     * (SDM, the local APIC's state after it has been software disabled), so
     * an unmasked `LINT0` written before this line would be dropped and the
     * 8259's interrupts would stop at the processor the moment the APIC came
     * up. The register is written as read, with the vector and the enable
     * (`lapic_svr`).
     */
    apic_write(LAPIC_SVR, lapic_svr(apic_read(LAPIC_SVR)));
    apic_write(LAPIC_TPR, 0);
    lint0 = apic_read(LAPIC_LINT0);
    if ((lint0 & LVT_MASKED) != 0 || (lint0 & LVT_DELIVERY_MASK) != LVT_DELIVERY_EXTINT) {
        /* Without this the 8259's interrupts stop at the processor. */
        apic_write(LAPIC_LINT0, LVT_DELIVERY_EXTINT);
        klog(KLOG_WARN, "lapic", "lint0 was 0x%x; set to an unmasked external interrupt", lint0);
    }
    ready = 1;
    klog(KLOG_DEBUG, "lapic", "ready: id %d, version 0x%x, base 0x%x", (int)apic_id,
         apic_read(LAPIC_VERSION) & 0xFFu, base);
    return 0;
}

/**
 * Whether `vector` is already waiting at this processor.
 *
 * A message the device has sent is latched in the request register until the
 * processor takes it, and is in the in-service register while its handler
 * runs. Either says the interrupt was announced, which is what the USB stack
 * asks before it counts an event as one no message was coming for: a
 * processor inside another interrupt has its flag clear, and a message that
 * arrived a moment earlier is waiting rather than lost.
 *
 * The two registers hold 256 bits each, sixteen to a 16-byte-spaced word.
 */
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
    return ready ? apic_read(LAPIC_ID) >> 24 : 0;
}

void lapic_eoi(void)
{
    if (ready) {
        apic_write(LAPIC_EOI, 0);
    }
}

/**
 * The spurious vector's handler: count it and return.
 *
 * The APIC raises vector 255 when an interrupt it was about to deliver went
 * away, which needs no end-of-interrupt and no work.
 */
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

/**
 * Whether `vector` is in this processor's request register: raised, and
 * not yet taken. Unlike `lapic_pending`, a vector whose handler is running
 * is not counted, which is what the 8259's request register says of a line.
 */
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

/**
 * Raise `vector` on this processor, as a device's message would: for an
 * edge an I/O APIC input could not see because it came while the input was
 * masked.
 */
void lapic_self_ipi(uint8_t vector)
{
    int spins;

    if (!ready) {
        return;
    }
    apic_write(LAPIC_ICR_HI, 0);
    apic_write(LAPIC_ICR_LO, ICR_SELF | vector);
    for (spins = 0; spins < 100000 && (apic_read(LAPIC_ICR_LO) & ICR_PENDING) != 0; spins++) {
        /* the delivery status clears once the message is sent */
    }
}

void lapic_init_ap(void)
{
    if (!ready) {
        return;
    }
    apic_write(LAPIC_SVR, lapic_svr(apic_read(LAPIC_SVR)));
    apic_write(LAPIC_TPR, 0);
}

/**
 * Write the interrupt command register for the processor `target`, the
 * destination in the high word first, since the write of the low word sends
 * the message, and wait for the APIC to have sent it. Interrupts are off
 * across the two writes: a handler that sent its own message between them
 * would leave this one with the other's destination.
 */
static void send_command(uint32_t target, uint32_t low)
{
    uint32_t flags;
    int spins;

    if (!ready) {
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

void lapic_send_init(uint32_t target)
{
    send_command(target, ICR_INIT | ICR_LEVEL | ICR_ASSERT);
}

void lapic_send_startup(uint32_t target, uint32_t page)
{
    send_command(target, ICR_STARTUP | (page & 0xFFu));
}

/**
 * Program this processor's timer: `vector`, periodic or one-shot, masked or
 * not, counting down from `initial` at the bus clock divided by sixteen.
 * The divide register is written before the initial count, which starts the
 * count.
 */
void lapic_timer_set(uint8_t vector, int periodic, int masked, uint32_t initial)
{
    if (!ready) {
        return;
    }
    apic_write(LAPIC_DIVIDE, DIVIDE_BY_16);
    apic_write(LAPIC_TIMER, vector | (periodic ? TIMER_PERIODIC : 0u) | (masked ? LVT_MASKED : 0u));
    apic_write(LAPIC_INITIAL, initial);
}

/** Stop this processor's timer and mask its entry. */
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
