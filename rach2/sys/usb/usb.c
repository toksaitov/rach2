/*
 * usb.c: the USB stack's entry points: the inventory of USB controllers, the
 * bring-up of every xHCI, the pump that drains them from the timer and from
 * their interrupts, and the stop.
 *
 * The inventory reads PCI configuration space alone, before any driver runs,
 * and writes nothing, so it cannot disturb a controller the firmware is still
 * driving: on a machine booted through the compatibility support module,
 * system management mode may be emulating a PS/2 keyboard from a USB one at
 * that time. On an Intel chipset with a companion EHCI the firmware decides
 * which of the two controllers owns each USB 2 pin, and the inventory logs
 * that routing as the firmware left it.
 */

#include "usb.h"

#include "../cmdline.h"
#include "../irq.h"
#include "../klog.h"
#include "../lib/kirq.h"
#include "../pci.h"
#include "../timer.h"
#include "../udelay.h"
#include "core.h"
#include "hcd.h"
#include "xhci.h"

/* The interface byte of a USB controller says which register interface it
 * presents. These are the four the standard defines for a host controller. */
#define USB_IF_UHCI 0x00
#define USB_IF_OHCI 0x10
#define USB_IF_EHCI 0x20
#define USB_IF_XHCI 0x30

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

const char *usb_interface_name(uint8_t prog_if)
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
 * Log one controller, with its interface byte beside the interface's name so
 * an unknown one is still identified, and its port routing when it is an
 * Intel xHCI.
 */
static void report_one(const pci_device_t *dev)
{
    klog(KLOG_INFO, "usb", "controller: %x:%x.%d %s interface 0x%x %x:%x bar0 0x%X", (int)dev->bus,
         (int)dev->device, (int)dev->function, usb_interface_name(dev->prog_if), (int)dev->prog_if,
         (int)dev->vendor_id, (int)dev->device_id, dev->bar[0]);

    if (dev->vendor_id == PCI_VENDOR_INTEL && dev->prog_if == USB_IF_XHCI) {
        uint32_t pr, prm, ss, ssm;

        pr = pci_read_config32(dev->bus, dev->device, dev->function, INTEL_XUSB2PR);
        prm = pci_read_config32(dev->bus, dev->device, dev->function, INTEL_XUSB2PRM);
        ss = pci_read_config32(dev->bus, dev->device, dev->function, INTEL_USB3PSSEN);
        ssm = pci_read_config32(dev->bus, dev->device, dev->function, INTEL_USB3PRM);
        klog(KLOG_INFO, "usb",
             "routing: usb2 0x%X of 0x%X, usb3 0x%X of 0x%X, as the firmware left it", pr, prm, ss,
             ssm);
    }
}

void usb_report_controllers(void)
{
    pci_device_t dev, prev;
    int have_prev = 0;
    int found = 0;

    while (pci_find_device_skip(PCI_CLASS_SERIAL_BUS, PCI_SUBCLASS_USB, have_prev ? &prev : NULL,
                                &dev)) {
        report_one(&dev);
        prev = dev;
        have_prev = 1;
        found++;
    }

    if (found == 0) {
        klog(KLOG_DEBUG, "usb", "controller: none");
    }
}

/* bringing the stack up */

/*
 * How long the ports are given to settle before they are read. The USB
 * specification requires a hundred milliseconds between a connect and a
 * reset, and a machine that has just started has every device already
 * connected.
 */
#define USB_SETTLE_MS 100u

/* How long the boot waits for what is already plugged in to be enumerated.
 * The timer steps each device once a tick, every 10 ms, and a device takes a
 * dozen steps, so this is enough for a few devices and short enough not to be
 * noticed; the timer finishes a device still being enumerated after it. */
#define USB_ENUMERATE_MS 400u

/*
 * Whether the stack is up. The timer calls `usb_poll` from the first tick of
 * the boot, and there is nothing to poll until a controller has been found and
 * started.
 */
static int usb_up;

static void usb_interrupts_start(void);

/* Each controller's interrupt source: its message vector or its line's IRQ,
 * -1 for the one it does not use, and both -1 when the timer alone pumps it. */
static struct {
    int msi_vector;
    int line_irq;
} interrupt_sources[XHCI_MAX_CONTROLLERS];
static int interrupt_source_count;

/* Interrupts handled, timer drains that took events with no interrupt
 * pending, and the interrupt count at which `rach2.msidrop` silences the
 * controllers, 0 when it is not set. */
static uint32_t usb_messages;
static uint32_t usb_fallback;
static uint32_t msi_drop_after;

/* Set after any interrupt source is enabled, including one later silenced. */
static int interrupts_started;

static int interrupt_pending(void);

/* Whether the controller had an interrupt raised for what the last pump
 * drained. The pump sets it; `usb_poll` reads it. */
static int pump_saw_pending;

/* Whether a pump was found to have enabled interrupts, so the record is
 * printed once and not every tick. */
static int pump_enabled_interrupts;

/* When the interrupt handler was last entered, in microseconds of
 * `timer_now_us`, for the latency record. */
static uint32_t stamp_entry;

/**
 * Let time pass at boot, which is the one place in this stack where that is
 * allowed.
 *
 * It waits and nothing more. The timer is already draining the controller and
 * stepping the devices, and a second party doing the same from here would be
 * two drains of one ring, one of them inside an interrupt of the other.
 */
static void usb_wait(uint32_t milliseconds)
{
    uint32_t ms;

    for (ms = 0; ms < milliseconds; ms++) {
        k_udelay(1000);
    }
}

/**
 * Record what is on each root port.
 *
 * A USB 2 port's speed field is valid only once the port is enabled, which
 * is after its reset; before that a low-speed device is told by its own
 * pull-up and a full- and a high-speed one read alike. The record says so,
 * and the device's own line later gives the speed the reset settled. The
 * lines are debug records, one per occupied port, since the lines that
 * follow name the devices themselves.
 */
static void report_ports(void)
{
    int found = 0;
    int n;

    for (n = 0; n < xhci_controllers(); n++) {
        struct xhci_ctrl *hc = xhci_controller(n);
        struct usb_port_status status;
        int ports = xhci_ports(hc);
        int port;

        for (port = 0; port < ports; port++) {
            if (xhci_port_status(hc, port, &status) != 0) {
                continue;
            }
            if (status.connected) {
                if (status.speed == USB_SPEED_LOW) {
                    klog(KLOG_DEBUG, "usb", "port: xhci%d.%d: a low-speed device", n, port + 1);
                } else {
                    klog(KLOG_DEBUG, "usb",
                         "port: xhci%d.%d: a device; its reset tells full from high speed", n,
                         port + 1);
                }
                found++;
            }
        }
    }
    if (found == 0) {
        klog(KLOG_INFO, "usb", "port: nothing connected");
    }
}

/** Whether interrupts are on, for the pump's check that it did not enable
 * them. */
static int usb_irq_on(void)
{
    return kirq_enabled();
}

/**
 * The one pump: advance every controller's clock, drain its event ring and
 * step the devices, from whichever context turns it.
 *
 * The event rings are drained here and nowhere else. The pump saves the
 * interrupt flag and puts it back rather than enabling interrupts on its way
 * out: the controller's interrupt handler and the timer's both run with
 * interrupts off, which keeps the two from ever being inside a ring at once
 * on this one processor, and the block layer's requests rely on that rule
 * when they sleep on a completion. A pump that finds interrupts on before it
 * restores the flag says so once, since one that does it does it every tick.
 *
 * Every controller is advanced and drained in each pass, so a machine with
 * two keeps both moving. The handler's drain acknowledges the interrupt first,
 * in the same call (`xhci_interrupt_take`); the timer's has nothing to
 * acknowledge.
 *
 * After a drain that took events, `pump_saw_pending` records whether an
 * interrupt for this stack is already pending, read at the interrupt
 * controller (`interrupt_pending`) and not at the xHCI, whose own pending bit
 * is set whether or not an interrupt was raised; one raised while the tick
 * held interrupts off waits in a request register. It is read after the
 * drain, because the controller answers a doorbell the step writes inside
 * that same write, so an event and its message can appear mid-pump, and read
 * again at the end: the event and its message are two transactions, and the
 * message may still be on its way when the event is in memory. Without that
 * a race would count as a lost edge.
 *
 * Returns the number of events taken.
 */
static int usb_pump(uint32_t elapsed_ms, int from_interrupt)
{
    uint32_t flags;
    int taken;

    flags = kirq_save();
    taken = 0;
    {
        int n;

        for (n = 0; n < xhci_controllers(); n++) {
            struct xhci_ctrl *hc = xhci_controller(n);
            int took;

            xhci_advance(hc, elapsed_ms);
            took = from_interrupt ? xhci_interrupt_take(hc) : xhci_events(hc);
            if (took > 0) {
                taken += took;
            }
        }
    }
    usb_stamp_pump(stamp_entry, taken > 0 ? timer_now_us() : 0, from_interrupt, taken > 0);
    if (taken > 0) {
        pump_saw_pending = interrupt_pending();
    }
    usb_core_step(elapsed_ms);
    if (taken > 0 && !pump_saw_pending) {
        pump_saw_pending = interrupt_pending();
    }
    if (usb_irq_on() && !pump_enabled_interrupts) {
        pump_enabled_interrupts = 1;
        klog(KLOG_ERR, "usb", "pump: interrupts were enabled inside it");
    }
    kirq_restore(flags);
    return taken;
}

/**
 * Whether any controller's interrupt has been raised and not yet taken.
 *
 * A controller's message vector is checked at the local APIC, and its PCI
 * line at the local APIC and the I/O APIC when the I/O APIC routes the
 * lines, or at the 8259 otherwise (`isr_line_pending`).
 */
static int interrupt_pending(void)
{
    int n;

    for (n = 0; n < interrupt_source_count; n++) {
        int irq = interrupt_sources[n].line_irq;
        int vector = interrupt_sources[n].msi_vector;

        if ((irq >= 0 && isr_line_pending(irq)) || (vector >= 0 && isr_vector_pending(vector))) {
            return 1;
        }
    }
    return 0;
}

/**
 * The controller's interrupt, as a message or on its line: acknowledge it,
 * pump, and return.
 *
 * The acknowledgement is the pump's first act when an interrupt turned it,
 * and no part of it when the timer did. An event the controller writes
 * between the acknowledgement and the drain is taken by that same drain,
 * which is why the two are one call in the driver.
 *
 * With `rach2.msidrop=N` the Nth interrupt silences every controller, and the
 * timer's pump is all that is left: it is how the fallback is seen to work on
 * a machine whose interrupts stop.
 *
 * The router ends a message, and a line the I/O APIC routes, at the local
 * APIC after this returns, and a line through the 8259 before this runs;
 * either way it then lets a woken reader run.
 */
static void usb_msi_handler(isr_params_t *params)
{
    (void)params;

    stamp_entry = timer_now_us();
    usb_messages++;
    (void)usb_pump(0, 1);
    if (msi_drop_after != 0 && usb_messages >= msi_drop_after) {
        int n;

        for (n = 0; n < xhci_controllers(); n++) {
            xhci_interrupts_disable(xhci_controller(n));
        }
        klog(KLOG_WARN, "usb", "msidrop: %d messages taken; the controllers were silenced",
             (int)usb_messages);
    }
}

/**
 * Which controller `rach2.hcfail=N` makes fail its bring-up, or -1.
 *
 * A controller that is present and will not start is the case the bring-up
 * loop exists for, and no QEMU machine has one. This makes the Nth controller
 * that would have come up fail instead, so what happens to the others can be
 * seen on a machine whose controllers all work.
 */
static int hcfail_at(void)
{
    char value[8];
    int n = 0;
    int i;

    if (!kcmdline_value("rach2.hcfail", value, (unsigned int)sizeof(value))) {
        return -1;
    }
    for (i = 0; value[i] >= '0' && value[i] <= '9'; i++) {
        n = n * 10 + (value[i] - '0');
    }
    return i > 0 && value[i] == '\0' ? n : -1;
}

/**
 * Each controller is brought up on its own. One whose bring-up fails is given
 * back to its firmware, which keeps whatever it was doing with it, and is
 * taken out of the table, so nothing reads its ports; the ones after it move
 * up, so the walk steps back onto the one that took its place. The others
 * still come up: a machine whose second controller is broken keeps the
 * sockets on its first. `rach2.hcfail=N` counts controllers in the order they
 * are tried, since a controller left out changes the index of every one
 * after it.
 *
 * From the stack's start the timer drains the controllers and steps the
 * devices, and this only waits: for the ports to settle, then for what is
 * plugged in to be enumerated, so a machine that boots with a keyboard has one
 * before the first process runs. The controllers' own interrupts come last,
 * since an interrupt raised while the rings were being built would have found
 * a stack that was not ready for it; a controller whose interrupts cannot be
 * enabled stays on the timer.
 */
int usb_init(void)
{
    int up = 0;
    int n;
    int seen = 0;

    xhci_dma32 = kcmdline_has("rach2.dma32");
    if (xhci_discover() != 0) {
        klog(KLOG_INFO, "usb", "absent: %s", xhci_error(0));
        return -1;
    }
    for (n = 0; n < xhci_controllers(); n++) {
        struct xhci_ctrl *hc = xhci_controller(n);
        const struct xhci_caps *caps;
        int refuse = (seen++ == hcfail_at());

        if (refuse || xhci_read_caps(hc) != 0 || xhci_own(hc) != 0 || xhci_halt_reset(hc) != 0 ||
            xhci_scratchpads(hc) != 0 || xhci_start(hc) != 0) {
            if (refuse) {
                klog(KLOG_WARN, "usb", "refused: xhci%d: rach2.hcfail=%d", n, n);
            } else {
                klog(KLOG_WARN, "usb", "refused: xhci%d: %s", n, xhci_error(hc));
            }
            xhci_release(hc);
            xhci_forget(hc);
            n--;
            continue;
        }
        caps = xhci_capabilities(hc);
        klog(KLOG_INFO, "usb", "ready: xhci%d: version %x, %d ports, %d slots, %d-byte contexts", n,
             (int)caps->hciversion, (int)caps->max_ports, (int)caps->max_slots,
             (int)caps->context_size);
        up++;
    }
    if (up == 0) {
        return -1;
    }

    usb_core_reset();
    usb_up = 1;

    usb_wait(USB_SETTLE_MS);
    report_ports();
    {
        uint32_t turn;

        for (turn = 0; turn < USB_ENUMERATE_MS && !usb_core_idle(); turn++) {
            k_udelay(1000);
        }
    }
    klog(KLOG_INFO, "usb", "devices: %d configured", usb_core_count());

    usb_interrupts_start();
    return 0;
}

/**
 * Take each controller's interrupts as messages, or on its line, or leave it
 * to the timer.
 *
 * Messages need the local APIC, which is off under `rach2.nomsi` and on a
 * processor without a usable one, a free vector, and an MSI capability the
 * controller can enable. Without them the controller raises its interrupts on
 * the PCI line the firmware routed for it, through the I/O APIC when one is
 * in use and the 8259 otherwise, as under `rach2.nomsi`; a stick moves data
 * many times faster that way than on the timer's pump alone, which finds each
 * completion at the next 10 ms tick. A controller with no line is left to the
 * timer.
 * `rach2.msidrop=N` silences the controllers after N interrupts, which is how
 * a machine that loses its interrupts is staged.
 *
 * Each controller gets a vector of its own. The handler is the one pump and
 * drains them all, so which vector a message arrived on does not matter, but
 * two controllers sharing a vector could not both be told to stop. On the
 * line path the handler is registered before the line is enabled, since a
 * level line the firmware left asserted would otherwise reach no handler.
 * The latency record is set up here too, on when `rach2.usblatency` is on
 * the boot line.
 */
static void usb_interrupts_start(void)
{
    char value[16];
    int irq;
    int n;

    interrupt_source_count = xhci_controllers();
    for (n = 0; n < interrupt_source_count; n++) {
        interrupt_sources[n].msi_vector = -1;
        interrupt_sources[n].line_irq = -1;
    }
    usb_stamp_init(kcmdline_has("rach2.usblatency"));
    if (kcmdline_value("rach2.msidrop", value, (unsigned int)sizeof(value))) {
        uint32_t after = 0;
        int i;

        for (i = 0; i < 6 && value[i] >= '0' && value[i] <= '9'; i++) {
            after = after * 10u + (uint32_t)(value[i] - '0');
        }
        msi_drop_after = after;
    }
    for (n = 0; n < xhci_controllers(); n++) {
        struct xhci_ctrl *hc = xhci_controller(n);
        int vector;

        if (isr_msi_ready() && (vector = isr_alloc_vector()) >= 0) {
            register_isr_handler((uint8_t)vector, usb_msi_handler, "xhci");
            if (xhci_interrupts_enable(hc, (uint8_t)vector) == 0) {
                interrupts_started = 1;
                interrupt_sources[n].msi_vector = vector;
                klog(KLOG_INFO, "usb", "interrupts: xhci%d: messages on vector %d", n, vector);
                continue;
            }
        }
        irq = xhci_line_irq(hc);
        if (irq > 0) {
            register_isr_handler((uint8_t)IRQ_VECTOR(irq), usb_msi_handler, "xhci");
            if (xhci_interrupts_enable_line(hc) == irq) {
                interrupts_started = 1;
                interrupt_sources[n].line_irq = irq;
                klog(KLOG_INFO, "usb", "interrupts: xhci%d: the line, irq %d on vector %d", n, irq,
                     IRQ_VECTOR(irq));
                continue;
            }
        }
        klog(KLOG_INFO, "usb", "interrupts: xhci%d: the timer pumps this controller", n);
    }
}

void usb_poll(void)
{
    if (!usb_up) {
        return;
    }
    pump_saw_pending = 0;
    if (usb_pump(10, 0) > 0 && interrupts_started && !pump_saw_pending) {
        usb_fallback++;
    }
}

uint32_t usb_fallback_count(void)
{
    return usb_fallback;
}

void usb_stop(void)
{
    int n;

    usb_up = 0;
    for (n = 0; n < xhci_controllers(); n++) {
        xhci_interrupts_disable(xhci_controller(n));
        xhci_stop(xhci_controller(n));
    }
}
