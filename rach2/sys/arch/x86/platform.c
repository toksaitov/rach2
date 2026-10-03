/*
 * platform.c: the PC's part of ../../platform.h, the order the boot brings
 * the machine's own devices up in kept by kernel.c and carried out here.
 */

#include "acpi.h"
#include "apicdiag.h"
#include "board.h"
#include "diag.h"
#include "ps2kbd.h"
#include "lapic.h"
#include "pciconf.h"
#include "ps2mouse.h"
#include "pit.h"

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

/* The most buses of segment 0 mapped for ECAM, a mebibyte each; a bus past
 * them is reached by mechanism #1. */
#define ECAM_MAX_BUSES 32


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
    a = acpi_get();
    ecam_init(a);
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

void platform_timer_start(isr_handler_t tick)
{
    arch_irq_disable();
    /* The local APIC, so that a device can be told to send its interrupts
     * as messages. A machine that refuses leaves every device on its legacy
     * line, which is the kernel as it was. Interrupts are off across it: it
     * moves the road the 8259's interrupts take to the processor, and there
     * is no reason to have one arrive in the middle of the move. */
    lapic_init();
    init_pit(100);
    set_isr_handler(32, tick, "pit"); /* install the timer handler on IRQ0 */
    arch_irq_enable();

    /*
     * The 8259's interrupts now reach the processor through the APIC's
     * `LINT0`, so the first thing the timer is asked is whether it still
     * arrives. A masked entry would otherwise show as a machine that boots
     * and never ticks, with nothing said about why. The wait is measured
     * against the 8254's own counter, which needs no interrupt.
     */
    if (lapic_ready()) {
        uint32_t before = kernel_ticks;
        int ms;

        for (ms = 0; ms < 50 && kernel_ticks == before; ms++) {
            k_udelay(1000);
        }
        if (kernel_ticks == before) {
            klog(KLOG_ERR, "lapic", "timer: no tick in 50 ms; writing lint0 again");
            lapic_repair_lint0();
            for (ms = 0; ms < 50 && kernel_ticks == before; ms++) {
                k_udelay(1000);
            }
            if (kernel_ticks == before) {
                klog(KLOG_ERR, "lapic",
                     "timer: still no tick; the 8259's interrupts are not arriving");
            }
        }
    }
}

uint32_t platform_timer_hz(void)
{
    return 1193182u / pit_reload;
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

void platform_keyboard_init(void)
{
    kbd_init();
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
