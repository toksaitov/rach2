/*
 * apicdiag.c: the `rach2.apicdiag` report, which reads and changes nothing.
 *
 * Before a kernel that takes interrupts through the local APIC and
 * message-signalled interrupts is installed on a machine, this says what
 * that machine's firmware left behind: whether the processor has an APIC
 * and the model-specific registers to reach it, where `IA32_APIC_BASE`
 * puts its page and whether it is in x2APIC mode, what the APIC's own
 * registers hold, whether `LINT0` is the unmasked external-interrupt entry
 * that keeps the 8259's interrupts arriving, and which of the two devices
 * this kernel drives by message have an MSI capability and where.
 *
 * It runs on the boot line so that the machine it is asked about is the
 * machine that answers. Milestone 43 runs it on the Lenovo before any
 * kernel with MSI is installed there.
 */

#include "apicdiag.h"

#include "../../cmdline.h"
#include "../../klog.h"
#include "kcpu.h"
#include "../../mm/paging.h"
#include "pci.h"

#define IA32_APIC_BASE 0x1B

#define LAPIC_ID      0x20
#define LAPIC_VERSION 0x30
#define LAPIC_TPR     0x80
#define LAPIC_SVR     0xF0
#define LAPIC_LINT0   0x350
#define LAPIC_LINT1   0x360

/* The MSI and MSI-X capabilities of one device, printed as they are. */
static void report_device(const char *what, const pci_device_t *dev)
{
    uint8_t msi, msix;

    klog(KLOG_INFO, "apicdiag", "%s: bus %x device %x function %d, vendor 0x%x device 0x%x, irq %d",
         what, dev->bus, dev->device, dev->function, dev->vendor_id, dev->device_id, dev->irq);
    msi = pci_find_capability(dev->bus, dev->device, dev->function, PCI_CAP_ID_MSI);
    msix = pci_find_capability(dev->bus, dev->device, dev->function, PCI_CAP_ID_MSIX);
    if (msi) {
        uint16_t control =
            pci_read_config16(dev->bus, dev->device, dev->function, (uint8_t)(msi + 2));

        klog(KLOG_INFO, "apicdiag", "%s: msi at 0x%x control 0x%x: %s, %d of %d vector(s), %s",
             what, msi, control, (control & 1u) ? "enabled" : "disabled",
             1 << ((control >> 4) & 7u), 1 << ((control >> 1) & 7u),
             (control & 0x80u) ? "64-bit address" : "32-bit address");
    } else {
        klog(KLOG_INFO, "apicdiag", "%s: no msi capability", what);
    }
    if (msix) {
        uint16_t control =
            pci_read_config16(dev->bus, dev->device, dev->function, (uint8_t)(msix + 2));

        klog(KLOG_INFO, "apicdiag", "%s: msi-x at 0x%x control 0x%x: %s, %d vector(s)", what, msix,
             control, (control & 0x8000u) ? "enabled" : "disabled", (control & 0x7FFu) + 1);
    } else {
        klog(KLOG_INFO, "apicdiag", "%s: no msi-x capability", what);
    }
}

void apic_diag_report(void)
{
    uint32_t lo = 0, hi = 0;
    uintptr_t page;
    pci_device_t dev;

    if (!kcmdline_has("rach2.apicdiag")) {
        return;
    }
    klog(KLOG_INFO, "apicdiag", "cpuid: apic %s, msr %s", kcpu_apic_present() ? "yes" : "no",
         kcpu_msr_present() ? "yes" : "no");
    if (!kcpu_apic_present() || !kcpu_msr_present()) {
        klog(KLOG_WARN, "apicdiag", "no local apic to read");
        return;
    }
    kcpu_rdmsr(IA32_APIC_BASE, &lo, &hi);
    klog(KLOG_INFO, "apicdiag", "IA32_APIC_BASE: hi 0x%x lo 0x%x: base 0x%x, %s%s%s", hi, lo,
         lo & 0xFFFFF000u, (lo & (1u << 11)) ? "enabled" : "disabled",
         (lo & (1u << 10)) ? ", x2apic" : "", (lo & (1u << 8)) ? ", bootstrap processor" : "");
    page = map_mmio((uintptr_t)(lo & 0xFFFFF000u), 0x1000);
    if (page == 0) {
        klog(KLOG_WARN, "apicdiag", "the apic page could not be mapped");
        return;
    }
    {
        volatile uint32_t *apic = (volatile uint32_t *)page;
        uint32_t id = apic[LAPIC_ID / 4];
        uint32_t version = apic[LAPIC_VERSION / 4];
        uint32_t lint0 = apic[LAPIC_LINT0 / 4];
        uint32_t lint1 = apic[LAPIC_LINT1 / 4];

        klog(KLOG_INFO, "apicdiag", "apic: id %d, version 0x%x, %d lvt entries", id >> 24,
             version & 0xFFu, ((version >> 16) & 0xFFu) + 1);
        klog(KLOG_INFO, "apicdiag", "apic: tpr 0x%x, svr 0x%x: %s, spurious vector %d",
             apic[LAPIC_TPR / 4], apic[LAPIC_SVR / 4],
             (apic[LAPIC_SVR / 4] & 0x100u) ? "software enabled" : "software disabled",
             apic[LAPIC_SVR / 4] & 0xFFu);
        klog(KLOG_INFO, "apicdiag", "apic: lint0 0x%x: %s, delivery %d%s", lint0,
             (lint0 & (1u << 16)) ? "masked" : "unmasked", (lint0 >> 8) & 7u,
             ((lint0 >> 8) & 7u) == 7 ? " (extint)" : "");
        klog(KLOG_INFO, "apicdiag", "apic: lint1 0x%x: %s, delivery %d%s", lint1,
             (lint1 & (1u << 16)) ? "masked" : "unmasked", (lint1 >> 8) & 7u,
             ((lint1 >> 8) & 7u) == 4 ? " (nmi)" : "");
    }
    if (pci_find_device(0x0C, 0x03, &dev) && dev.prog_if == 0x30) {
        report_device("xhci", &dev);
    } else {
        klog(KLOG_INFO, "apicdiag", "xhci: no controller on the bus");
    }
    if (pci_find_device(0x04, 0x03, &dev)) {
        report_device("hda", &dev);
    } else {
        klog(KLOG_INFO, "apicdiag", "hda: no controller on the bus");
    }
    klog(KLOG_INFO, "apicdiag", "report: done, nothing written");
}
