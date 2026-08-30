/*
 * usb.c: what USB hardware this machine has.
 *
 * The first piece of the USB stack, and for now the whole of it: an inventory
 * taken from PCI configuration space. It exists before the driver because the
 * two machines this kernel runs on answer differently, and the difference
 * decides what the driver must do. QEMU presents whichever controller the
 * command line asked for. The notebook presents an Intel Lynx Point-LP xHCI
 * and a companion EHCI, and its firmware decides which of the two owns each
 * USB2 pin.
 *
 * Nothing here writes. Reading configuration space cannot disturb a controller
 * the firmware is still driving, which matters on a machine booted through the
 * compatibility support module, where system management mode may be emulating
 * a PS/2 keyboard out of a USB one at the time this runs.
 */

#include "usb.h"

#include "../klog.h"
#include "../pci.h"

/* The interface byte of a USB controller says which register interface it
 * presents. These are the four the standard defines, plus the value a USB
 * device on the bus reports. */
#define USB_IF_UHCI   0x00
#define USB_IF_OHCI   0x10
#define USB_IF_EHCI   0x20
#define USB_IF_XHCI   0x30

/* Intel routes the USB2 and USB3 pins between the xHCI and its companion EHCI
 * through four registers in the xHCI's own configuration space. The mask
 * registers say which pins may be routed at all; the other two say where each
 * routable pin currently goes. 8 Series PCH datasheet, xHCI configuration
 * registers. */
#define INTEL_XUSB2PR   0xD0
#define INTEL_XUSB2PRM  0xD4
#define INTEL_USB3PSSEN 0xD8
#define INTEL_USB3PRM   0xDC

#define PCI_VENDOR_INTEL 0x8086

/**
 * The name of a register interface, or `other` for one this kernel does not
 * know. The byte itself is recorded beside the name, so an unknown controller
 * is still identified by the record.
 */
static const char *interface_name(uint8_t prog_if)
{
    switch (prog_if) {
    case USB_IF_UHCI:
        return "uhci";
    case USB_IF_OHCI:
        return "ohci";
    case USB_IF_EHCI:
        return "ehci";
    case USB_IF_XHCI:
        return "xhci";
    default:
        return "other";
    }
}

/**
 * Record one controller, and its port routing when it is an Intel xHCI.
 */
static void report_one(const pci_device_t *dev)
{
    klog(KLOG_INFO, "usb", "controller: %x:%x.%d %s interface 0x%x %x:%x bar0 0x%X",
         (int)dev->bus, (int)dev->device, (int)dev->function, interface_name(dev->prog_if),
         (int)dev->prog_if, (int)dev->vendor_id, (int)dev->device_id, dev->bar[0]);

    if (dev->vendor_id == PCI_VENDOR_INTEL && dev->prog_if == USB_IF_XHCI) {
        uint32_t pr, prm, ss, ssm;

        pr = pci_read_config32(dev->bus, dev->device, dev->function, INTEL_XUSB2PR);
        prm = pci_read_config32(dev->bus, dev->device, dev->function, INTEL_XUSB2PRM);
        ss = pci_read_config32(dev->bus, dev->device, dev->function, INTEL_USB3PSSEN);
        ssm = pci_read_config32(dev->bus, dev->device, dev->function, INTEL_USB3PRM);
        klog(KLOG_INFO, "usb", "routing: usb2 0x%X of 0x%X, usb3 0x%X of 0x%X",
             pr, prm, ss, ssm);
    }
}

void usb_report_controllers(void)
{
    pci_device_t dev, prev;
    int have_prev = 0;
    int found = 0;

    while (pci_find_device_skip(PCI_CLASS_SERIAL_BUS, PCI_SUBCLASS_USB,
                                have_prev ? &prev : NULL, &dev)) {
        report_one(&dev);
        prev = dev;
        have_prev = 1;
        found++;
    }

    if (found == 0) {
        klog(KLOG_INFO, "usb", "controller: none");
    }
}
