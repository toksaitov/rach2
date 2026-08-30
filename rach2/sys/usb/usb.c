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
#include "../udelay.h"
#include "hcd.h"
#include "xhci.h"

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

/* ---- bringing the stack up ---- */

/*
 * How long the ports are given to settle before they are read, and how often
 * the driver's clock is advanced while that happens. The USB specification
 * asks for a hundred milliseconds between a connect and anything done about
 * it, and a machine that has just started has every device already connected.
 */
#define USB_SETTLE_MS 100u

/** The name of a speed, for the record. */
static const char *speed_name(int speed)
{
    switch (speed) {
    case USB_SPEED_FULL:
        return "full";
    case USB_SPEED_LOW:
        return "low";
    case USB_SPEED_HIGH:
        return "high";
    case USB_SPEED_SUPER:
        return "super";
    default:
        return "unknown";
    }
}

/**
 * Let time pass at boot, which is the one place in this stack where that is
 * allowed, and keep the controller drained while it does.
 */
static void usb_settle(void)
{
    uint32_t ms;

    for (ms = 0; ms < USB_SETTLE_MS; ms++) {
        k_udelay(1000);
        xhci_advance(1);
        xhci_events();
    }
}

/** Record what is on each root port. */
static void report_ports(void)
{
    struct usb_port_status status;
    int ports = xhci_ports();
    int found = 0;
    int port;

    for (port = 0; port < ports; port++) {
        if (xhci_port_status(port, &status) != 0) {
            continue;
        }
        if (status.connected) {
            klog(KLOG_INFO, "usb", "port: %d: a %s-speed device", port + 1,
                 speed_name(status.speed));
            found++;
        }
    }
    if (found == 0) {
        klog(KLOG_INFO, "usb", "port: nothing connected");
    }
}

int usb_init(void)
{
    const struct xhci_caps *caps;

    if (xhci_discover() != 0) {
        klog(KLOG_INFO, "usb", "absent: %s", xhci_error());
        return -1;
    }
    if (xhci_read_caps() != 0 || xhci_own() != 0 || xhci_halt_reset() != 0 ||
        xhci_scratchpads() != 0 || xhci_start() != 0) {
        klog(KLOG_WARN, "usb", "refused: %s", xhci_error());
        return -1;
    }

    caps = xhci_capabilities();
    klog(KLOG_INFO, "usb", "ready: version %x, %d ports, %d slots, %d-byte contexts",
         (int)caps->hciversion, (int)caps->max_ports, (int)caps->max_slots,
         (int)caps->context_size);

    usb_settle();
    report_ports();
    return 0;
}

void usb_poll(void)
{
    xhci_advance(10);
    xhci_events();
}

void usb_stop(void)
{
    xhci_stop();
}
