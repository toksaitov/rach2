/*
 * platform.c: the PC's part of ../../platform.h, the order the boot brings
 * the machine's own devices up in kept by kernel.c and carried out here.
 *
 * The interrupt controllers are one of two arrangements, chosen once, in
 * `platform_timer_start`. When the MADT lists an I/O APIC, the local APIC is
 * up and `noapic` is not on the boot line, the I/O APIC takes lines 0 to 15
 * through the MADT's overrides, the 8259 pair is masked for good, and the
 * tick is the local APIC's timer, measured against the HPET, the PM timer or
 * the 8254. Otherwise the 8259 pair delivers the lines and the 8254 ticks,
 * through the local APIC's `LINT0` when it is enabled for messages. Either
 * way a line arrives on the vector it always had, 32 plus the line.
 */

#include "acpi.h"
#include "apicdiag.h"
#include "apictimer.h"
#include "board.h"
#include "diag.h"
#include "ioapic.h"
#include "ps2kbd.h"
#include "lapic.h"
#include "pciconf.h"
#include "pic.h"
#include "portio.h"
#include "ps2mouse.h"
#include "pit.h"
#include "refclock.h"
#include "smbios.h"
#include "smpboot.h"

#include "../arch.h"
#if defined(__x86_64__)
#include "../x86_64/percpu.h"
#else
#include "../i686/gdt.h"
#include "../i686/idt.h"
#include "../i686/tss.h"
#endif

#include "../../audio/sb16.h"
#include "../../block/ata.h"
#include "../../boot.h"
#include "../../cmdline.h"
#include "../../input.h"
#include "../../klog.h"
#include "../../mm/paging.h"
#include "../../pci.h"
#include "../../platform.h"
#include "../../timer.h"
#include "../../udelay.h"

#include "../../lib/kirq.h"

/* The kernel's tick rate. */
#define TICK_HZ 100

/* The most buses of segment 0 mapped for ECAM, a mebibyte each; a bus past
 * them is reached by mechanism #1. */
#define ECAM_MAX_BUSES 32

/* Whether the local APIC's timer is the tick, with its counts a tick,
 * which a started processor's timer is given too, and the lines PCI devices
 * name, which the I/O APIC takes level-triggered. */
static int tick_local;
static uint32_t tick_counts;
static uint16_t lines_pci;

uint32_t x86_tick_counts(void)
{
    return tick_local ? tick_counts : 0;
}

void platform_cpu_init(uintptr_t boot_stack_top)
{
    init_gdt();
    klog(KLOG_DEBUG, "boot", "gdt: ready");

#if defined(__x86_64__)
    init_tss(boot_stack_top);
#else
    init_tss((uint32_t)boot_stack_top);
#endif
    klog(KLOG_DEBUG, "boot", "tss: ready");

    init_idt();
    klog(KLOG_DEBUG, "boot", "idt: ready");

    init_isr();
    klog(KLOG_DEBUG, "boot", "isr: ready");
}

/** The functions device `dev` of `bus` has: 0 when it does not answer, 8
 * when its header says it has several, 1 otherwise. */
static uint8_t pci_functions(uint8_t bus, uint8_t dev)
{
    if (pci_read_config16(bus, dev, 0, PCI_VENDOR_ID) == 0xFFFFu) {
        return 0;
    }
    return (pci_read_config8(bus, dev, 0, PCI_HEADER_TYPE) & 0x80u) ? 8u : 1u;
}

/**
 * Call `visit` for each function that answers on buses `first` to `*last`,
 * by whichever configuration mechanism serves each bus. `*last` is read
 * again after each bus, so a visit may move it on.
 */
static void pci_each(uint32_t first, uint32_t *last,
                     void (*visit)(uint8_t bus, uint8_t dev, uint8_t fn, void *arg), void *arg)
{
    uint32_t bus;
    uint8_t dev, fn, n;

    for (bus = first; bus <= *last && bus < 256u; bus++) {
        for (dev = 0; dev < 32; dev++) {
            n = pci_functions((uint8_t)bus, dev);
            for (fn = 0; fn < n; fn++) {
                if (pci_read_config16((uint8_t)bus, dev, fn, PCI_VENDOR_ID) != 0xFFFFu) {
                    visit((uint8_t)bus, dev, fn, arg);
                }
            }
        }
    }
}

/* The buses a walk reaches through bridges, and the most it may. */
struct bus_reach {
    uint32_t top;
    uint32_t cap;
};

static void reach_bridge(uint8_t bus, uint8_t dev, uint8_t fn, void *arg)
{
    struct bus_reach *r = (struct bus_reach *)arg;
    uint32_t sub;

    if ((pci_read_config8(bus, dev, fn, PCI_HEADER_TYPE) & 0x7Fu) != 1u) {
        return;
    }
    sub = pci_read_config8(bus, dev, fn, 0x1A); /* the subordinate bus */
    if (sub > r->top) {
        r->top = sub > r->cap ? r->cap : sub;
    }
}

static void count_extended(uint8_t bus, uint8_t dev, uint8_t fn, void *arg)
{
    uint32_t cap = pci_read_config32_ext(bus, dev, fn, 0x100);

    if (cap != 0 && cap != 0xFFFFFFFFu) {
        (*(int *)arg)++;
        klog(KLOG_DEBUG, "pci", "ecam: %x:%x.%d: extended capability 0x%x, version %d, at 0x100",
             (unsigned)bus, (unsigned)dev, (int)fn, cap & 0xFFFFu, (int)((cap >> 16) & 0xFu));
    }
}

/**
 * Map segment 0's ECAM window for the buses that exist, the MCFG's first
 * and those behind the bridges on it, read through mechanism #1 before the
 * window is in place, and at most `ECAM_MAX_BUSES`; and hand it to the
 * configuration functions once its first function reads as mechanism #1
 * reads it. Each function whose extended space begins with a capability is
 * logged, which is the space only ECAM reaches.
 */
static void ecam_init(const struct acpi_info *a)
{
    const struct acpi_ecam *e = 0;
    struct bus_reach r;
    uint64_t base;
    uintptr_t va;
    int i, extended = 0;

    for (i = 0; i < a->necam && !e; i++) {
        if (a->ecam[i].segment == 0) {
            e = &a->ecam[i];
        }
    }
    if (!e) {
        return;
    }
    r.top = e->bus_first;
    r.cap = e->bus_first + ECAM_MAX_BUSES - 1u;
    if (r.cap > e->bus_last) {
        r.cap = e->bus_last;
    }
    pci_each(e->bus_first, &r.top, reach_bridge, &r);
    base = e->base + ((uint64_t)e->bus_first << 20);
    if (sizeof(uintptr_t) < 8 &&
        ((base + ((uint64_t)(r.top - e->bus_first + 1u) << 20) - 1u) >> 32) != 0) {
        klog(KLOG_WARN, "pci", "ecam: refused: the window is above 4 gib");
        return;
    }
    va = map_mmio((uintptr_t)base, (size_t)(r.top - e->bus_first + 1u) << 20);
    if (va == 0) {
        klog(KLOG_WARN, "pci", "ecam: refused: the window could not be mapped");
        return;
    }
    if (*(volatile uint32_t *)va != pci_read_config32(e->bus_first, 0, 0, 0)) {
        klog(KLOG_WARN, "pci", "ecam: refused: bus %d's first function reads otherwise there",
             (int)e->bus_first);
        return;
    }
    pci_ecam_set(va, e->bus_first, (uint8_t)r.top);
    klog(KLOG_INFO, "pci", "ecam: buses %d to %d of segment 0 at 0x%x", (int)e->bus_first,
         (int)r.top, (uint32_t)base);
    pci_each(e->bus_first, &r.top, count_extended, &extended);
    klog(KLOG_INFO, "pci", "ecam: %d functions with extended capabilities", extended);
}

void platform_firmware_init(const struct boot_info *boot)
{
    const struct acpi_info *a;

    acpi_init(boot->acpi_rsdp);
    smbios_init(boot->efi_systab, boot->efi_systab_64);
    a = acpi_get();
    ecam_init(a);
    (void)refclock_init(a);
    smpboot_init(boot);
}

static void note_line(uint8_t bus, uint8_t dev, uint8_t fn, void *arg)
{
    uint8_t pin = pci_read_config8(bus, dev, fn, PCI_INTERRUPT_PIN);
    uint8_t line = pci_read_config8(bus, dev, fn, PCI_INTERRUPT_LINE);

    if (pin != 0 && line < 16) {
        *(uint16_t *)arg |= (uint16_t)(1u << line);
    }
}

/**
 * The lines a PCI function's interrupt line register gives, for a function
 * that has an interrupt pin: the ISA lines firmware routed the PIRQs to,
 * which the I/O APIC takes level-triggered.
 */
static uint16_t pci_lines(void)
{
    uint16_t lines = 0;
    uint32_t last = 255;

    pci_each(0, &last, note_line, &lines);
    return lines;
}

/**
 * Whether the I/O APIC is to take the lines, with the reason in the log
 * when it is not, and its inputs masked when it is.
 */
static int ioapic_wanted(const struct acpi_info *a)
{
    if (kcmdline_has("noapic")) {
        klog(KLOG_INFO, "ioapic", "skipped: noapic");
        return 0;
    }
    if (!lapic_ready()) {
        klog(KLOG_INFO, "ioapic", "skipped: the local apic is not in use");
        return 0;
    }
    if (!a->madt || a->nioapics == 0) {
        klog(KLOG_INFO, "ioapic", "none: the firmware lists no ioapic");
        return 0;
    }
    return ioapic_init(a) > 0;
}

void platform_report(void)
{
    /* The boot report, after the page cache's window is mapped and before
     * the disk and network drivers have configured a device. After, because
     * the window's page tables and the report's own device peek take
     * memory, and a figure printed before those is not what the machine has
     * left. */
    if (kcmdline_has("rach2.diag")) {
        boot_diag_report();
    }

    /* What the machine's firmware left in the APIC and in the two devices
     * this kernel drives by message, read and printed, nothing written. */
    apic_diag_report();
}

void platform_report_hold(void)
{
    boot_diag_hold();
}

/** Spin until the tick counter moves from `before`, 50 ms at most by the
 * 8254 or the reference counter: whether it moved. */
static int tick_arrives(uint32_t before)
{
    int ms;

    for (ms = 0; ms < 50 && kernel_ticks == before; ms++) {
        k_udelay(1000);
    }
    return kernel_ticks != before;
}

void platform_timer_start(isr_handler_t tick)
{
    const struct acpi_info *a = acpi_get();
    const char *against = "";
    uint32_t counts;
    int apic;

    arch_irq_disable();
    /* The local APIC, so that a device can be told to send its interrupts
     * as messages. A machine that refuses leaves every device on its legacy
     * line, which is the kernel as it was. Interrupts are off across it: it
     * moves the road the 8259's interrupts take to the processor, and there
     * is no reason to have one arrive in the middle of the move. */
    lapic_init();
    init_pit(TICK_HZ);
    if (!pit_probe()) {
        klog(KLOG_WARN, "pit", "none: channel 0 does not count");
        if (refclock_name()) {
            udelay_wait = refclock_wait_us;
        }
    }
    apic = ioapic_wanted(a);
    if (apic) {
        counts = apic_timer_calibrate(TICK_HZ, &against);
        /* The 8259 first, so that its request to the processor falls
         * while `LINT0` still takes it, and then `LINT0`. */
        pic_retire();
        apic_lint_init(a);
        lines_pci = pci_lines();
        isr_lines_to_apic(lines_pci, counts ? 1u : 0u);
        if (counts) {
            tick_local = 1;
            tick_counts = counts;
            timer_set_counter(&apic_timer_counter);
            set_isr_handler(APIC_TIMER_VECTOR, tick, "timer");
            apic_timer_start(counts, TICK_HZ);
            klog(KLOG_INFO, "lapic", "tick: %d counts, measured against the %s", (int)counts,
                 against);
        } else {
            timer_set_counter(&apic_pit_counter);
            set_isr_handler(IRQ_VECTOR(0), tick, "pit");
            klog(KLOG_WARN, "lapic", "tick: not measured against the %s; the pit ticks", against);
        }
        klog(KLOG_INFO, "ioapic", "interrupts: lines through the ioapic, the 8259 masked");
    } else {
        set_isr_handler(IRQ_VECTOR(0), tick, "pit"); /* install the timer handler on IRQ0 */
    }
    arch_irq_enable();

    if (apic) {
        /* A timer measured wrong, or one that does not interrupt, would be
         * a machine that boots and never ticks. The 8254 through the I/O
         * APIC is what is left. */
        if (tick_arrives(kernel_ticks)) {
            return;
        }
        if (tick_local) {
            klog(KLOG_ERR, "lapic", "timer: no tick in 50 ms; the pit ticks instead");
            arch_irq_disable();
            lapic_timer_stop();
            tick_local = 0;
            timer_set_counter(&apic_pit_counter);
            isr_lines_to_apic(lines_pci, 0);
            set_isr_handler(IRQ_VECTOR(0), tick, "pit");
            arch_irq_enable();
            if (tick_arrives(kernel_ticks)) {
                return;
            }
        }
        klog(KLOG_ERR, "lapic", "timer: still no tick; the pit's line does not arrive either");
        return;
    }

    /*
     * The 8259's interrupts now reach the processor through the APIC's
     * `LINT0`, so the first thing the timer is asked is whether it still
     * arrives. A masked entry would otherwise show as a machine that boots
     * and never ticks, with nothing said about why. The wait is measured
     * against the 8254's own counter, which needs no interrupt.
     */
    if (lapic_ready() && !tick_arrives(kernel_ticks)) {
        klog(KLOG_ERR, "lapic", "timer: no tick in 50 ms; writing lint0 again");
        lapic_repair_lint0();
        if (!tick_arrives(kernel_ticks)) {
            klog(KLOG_ERR, "lapic", "timer: still no tick; the 8259's interrupts are not arriving");
        }
    }
}

uint32_t platform_timer_hz(void)
{
    return tick_local ? TICK_HZ : 1193182u / pit_reload;
}

void platform_board_init(void)
{
    /* Which power controller this machine has. With ACPI on, an unhandled
     * SCI is level-triggered and would be taken forever, so the line it sits
     * on is masked here. A machine no profile matches keeps exactly the
     * behaviour it had before this existed. */
    const char *why = "";
    int verdict = board_init(&why);

    if (verdict == BOARD_FOUND) {
        klog(KLOG_INFO, "board", "ready: %s", board_name());
    } else if (verdict == BOARD_REFUSED) {
        klog(KLOG_WARN, "board", "refused: %s", why);
    } else {
        klog(KLOG_WARN, "board", "none: no profile matched; poweroff will halt");
    }
}

/**
 * Take a byte the 8042 has waiting, by raising its vector once, after the
 * handler went on through the I/O APIC. The 8042 keeps its line high while
 * a byte waits and raises no edge for another until that one is read, and
 * an I/O APIC input drops an edge that comes while it is masked, where the
 * 8259 kept it in its request register; so a key pressed before the
 * handler would leave the keyboard silent for good. `aux` asks after the
 * mouse's byte rather than the keyboard's.
 */
static void ps2_waiting(uint8_t vector, int aux)
{
    uint32_t flags;
    uint8_t status;

    if (!isr_lines_apic()) {
        return;
    }
    flags = kirq_save();
    status = inb(0x64);
    if (status != 0xFF && (status & 0x01u) != 0 && ((status & 0x20u) != 0) == (aux != 0) &&
        !lapic_requested(vector)) {
        lapic_self_ipi(vector);
    }
    kirq_restore(flags);
}

void platform_keyboard_init(void)
{
    kbd_init();
    ps2_waiting(IRQ_VECTOR(1), 0);
}

void platform_mouse_init(void)
{
    if (kcmdline_has("rach2.nomouse")) {
        klog(KLOG_INFO, "mouse", "skipped: rach2.nomouse");
        return;
    }
    klog(KLOG_DEBUG, "mouse", "started");
    if (init_mouse() == 0) {
        input_mouse_found();
        ps2_waiting(IRQ_VECTOR(12), 1);
        klog(KLOG_DEBUG, "mouse", "ready");
    } else {
        klog(KLOG_WARN, "mouse", "absent: nothing answered the probe");
    }
}

void platform_tick(void)
{
    ata_tick();
}

void platform_disks_probe(void)
{
    (void)ata_init();
}

struct audio_driver *platform_audio_driver(const char **name)
{
    *name = "Sound Blaster 16";
    return &sb16_driver;
}
