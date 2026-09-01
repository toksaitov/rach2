/*
 * lapic.c: the local APIC, for message-signalled interrupts.
 *
 * A message from a PCI device is a write to the APIC's address, so the APIC
 * must be enabled before any device is told to send one. Nothing else here
 * uses it: the timer stays the PIT, the 8259 pair still delivers the
 * keyboard, the mouse and the disks, and the IOAPIC is not programmed.
 *
 * The 8259's interrupts reach the processor through `LINT0` as an external
 * interrupt, the state firmware calls virtual wire. Enabling the APIC with
 * `LINT0` masked would silence the timer and the keyboard, so this driver
 * reads that entry and writes the unmasked external-interrupt value when it
 * finds anything else. `LINT1` is left as the firmware set it.
 *
 * A processor without an APIC, without the model-specific registers, in
 * x2APIC mode, or with its registers somewhere other than the page this
 * driver maps, is refused by name: the kernel then has no message vectors
 * and every device stays on its legacy line. `rach2.nomsi` refuses in the
 * same way, so a machine that misbehaves has one word on its boot line.
 */

#include "lapic.h"

#include "cmdline.h"
#include "klog.h"
#include "lib/kcpu.h"
#include "mm/paging.h"

#define IA32_APIC_BASE   0x1B
#define APIC_BASE_ENABLE (1u << 11)
#define APIC_BASE_X2APIC (1u << 10)

#define LAPIC_ID      0x20
#define LAPIC_VERSION 0x30
#define LAPIC_TPR     0x80
#define LAPIC_EOI     0xB0
#define LAPIC_SVR     0xF0
#define LAPIC_LINT0   0x350
#define LAPIC_LINT1   0x360

#define LVT_MASKED          (1u << 16)
#define LVT_DELIVERY_EXTINT (7u << 8)
#define SVR_SOFTWARE_ENABLE 0x100u
#define SPURIOUS_VECTOR     0xFFu

/* The page the APIC's registers live on, and zero until `lapic_init` maps
 * it, which is what `lapic_ready` reports and every write here checks. */
static volatile uint32_t *apic;
static const struct lapic_io *io;
static uint32_t apic_id;
static int ready;
static uint32_t spurious_count;

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
 * The order matters: the task priority goes to zero and `LINT0` is made an
 * unmasked external-interrupt entry before the spurious register's software
 * enable, so that no interrupt is blocked or lost in between. 0 when the
 * APIC is ready to receive messages, -1 when it is not.
 */
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
    apic_write(LAPIC_TPR, 0);
    lint0 = apic_read(LAPIC_LINT0);
    if ((lint0 & LVT_MASKED) != 0 || (lint0 & (7u << 8)) != LVT_DELIVERY_EXTINT) {
        /* Without this the 8259's interrupts stop at the processor. */
        apic_write(LAPIC_LINT0, LVT_DELIVERY_EXTINT);
        klog(KLOG_WARN, "lapic", "lint0 was 0x%x; set to an unmasked external interrupt", lint0);
    }
    apic_write(LAPIC_SVR, SVR_SOFTWARE_ENABLE | SPURIOUS_VECTOR);
    ready = 1;
    klog(KLOG_INFO, "lapic", "ready: id %d, version 0x%x, base 0x%x", apic_id,
         apic_read(LAPIC_VERSION) & 0xFFu, base);
    return 0;
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
    spurious_count++;
}

uint32_t lapic_spurious_count(void)
{
    return spurious_count;
}
