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

#include "../cmdline.h"
#include "../pic.h"
#include "../isr.h"
#include "../klog.h"
#include "../lapic.h"
#include "../lib/kirq.h"
#include "../pci.h"
#include "../pit.h"
#include "../udelay.h"
#include "core.h"
#include "hcd.h"
#include "xhci.h"

/* The interface byte of a USB controller says which register interface it
 * presents. These are the four the standard defines, plus the value a USB
 * device on the bus reports. */
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

/**
 * The name of a register interface, or `other` for one this kernel does not
 * know. The byte itself is recorded beside the name, so an unknown controller
 * is still identified by the record.
 */
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
 * Record one controller, and its port routing when it is an Intel xHCI.
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

/* ---- bringing the stack up ---- */

/*
 * How long the ports are given to settle before they are read, and how often
 * the driver's clock is advanced while that happens. The USB specification
 * asks for a hundred milliseconds between a connect and anything done about
 * it, and a machine that has just started has every device already connected.
 */
#define USB_SETTLE_MS 100u

/* How long the boot waits for what is already plugged in to be enumerated.
 * Each turn is one step of the state machine and one millisecond, and a device
 * takes a dozen steps, so this is generous for the four devices this kernel
 * holds and short enough not to be noticed. */
#define USB_ENUMERATE_MS 400u

/*
 * Whether the stack is up. The timer calls `usb_poll` from the first tick of
 * the boot, and there is nothing to poll until a controller has been found and
 * started.
 */
static int usb_up;

static void usb_interrupts_start(void);

/** The configured interrupt source of each controller, or neither. */
static struct {
    int msi_vector;
    int line_irq;
} interrupt_sources[XHCI_MAX_CONTROLLERS];
static int interrupt_source_count;

/* Interrupts handled, timer drains without observed pending interrupts,
 * and the interrupt count at which rach2.msidrop disables delivery. */
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
 * and the device's own line later carries the speed the reset settled.
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
                /* One line per occupied port, before anything is known about
                   what is on it. A laptop has six of these and the lines that
                   follow name the devices themselves, so this is for whoever
                   is debugging enumeration. */
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

/*
 * The one pump, and the two contexts that turn it.
 *
 * The controller's event ring is drained here and nowhere else. Whichever
 * context turns it, the interrupt flag stays as that context found it: the
 * handler already runs with interrupts off through its gate, the timer's
 * handler does too, and this saves and restores rather than enabling on its
 * way out. That is what keeps the two from ever being inside the ring at once
 * on this one processor, and it is the rule the block layer's requests will
 * rely on when they sleep on a completion.
 */
static uint32_t usb_eflags(void)
{
#ifdef RACH2_HOSTTEST
    uint32_t flags = kirq_save();

    kirq_restore(flags);
    return flags;
#else
    uint32_t flags;

    __asm__ __volatile__("pushf\n\t"
                         "pop %0"
                         : "=r"(flags)
                         :
                         : "memory");
    return flags;
#endif
}

static int usb_pump(uint32_t elapsed_ms, int from_interrupt)
{
    uint32_t flags;
    int taken;

    flags = kirq_save();
    /* Every controller is advanced and drained in the one pump, so a machine
     * with two of them keeps both moving from whichever context turns it. */
    taken = 0;
    {
        int n;

        for (n = 0; n < xhci_controllers(); n++) {
            struct xhci_ctrl *hc = xhci_controller(n);
            int took;

            xhci_advance(hc, elapsed_ms);
            /* The handler's drain acknowledges the interrupt first and in the
             * same call; the timer's has nothing to acknowledge. */
            took = from_interrupt ? xhci_interrupt_take(hc) : xhci_events(hc);
            if (took > 0) {
                taken += took;
            }
        }
    }
    usb_stamp_pump(stamp_entry, taken > 0 ? pit_now_us() : 0, from_interrupt, taken > 0);
    if (taken > 0) {
        /*
         * Whether a message for this stack is already waiting at the
         * processor. Asked here and not before the drain: the controller
         * answers a doorbell written by the step above inside that same
         * write, so an event can appear, and a message be sent for it, in the
         * middle of this pump. The processor is what is asked and not the
         * controller: the controller's pending bit is set whether or not a
         * message was sent, and a message that was sent while this tick held
         * the interrupt flag down is waiting in the processor's own request
         * register.
         */
        pump_saw_pending = interrupt_pending();
    }
    usb_core_step(elapsed_ms);
    if (taken > 0 && !pump_saw_pending) {
        /*
         * Asked once more at the end of the pump. The controller writes the
         * event and sends the message as two transactions, and the second
         * can still be on its way to the processor while the first is
         * already in memory; the work of the pump is time enough for it to
         * arrive, and without this the record would count a race as an edge
         * that was lost.
         */
        pump_saw_pending = interrupt_pending();
    }
    /* The flag is read before it is put back: the pump must not have enabled
     * interrupts, or the timer's context and the handler's would be inside
     * the controller's ring at once on this one processor. Said once, since a
     * pump that did this does it every tick. */
    if ((usb_eflags() & 0x200u) != 0 && !pump_enabled_interrupts) {
        pump_enabled_interrupts = 1;
        klog(KLOG_ERR, "usb", "pump: interrupts were enabled inside it");
    }
    kirq_restore(flags);
    return taken;
}

/**
 * Return whether any configured USB interrupt source is pending.
 *
 * Check each controller's MSI vector or PCI IRQ at the processor.
 */
static int interrupt_pending(void)
{
    int n;

    for (n = 0; n < interrupt_source_count; n++) {
        int irq = interrupt_sources[n].line_irq;
        int vector = interrupt_sources[n].msi_vector;

        if ((irq >= 0 && pic_irq_pending((uint8_t)irq)) ||
            (vector >= 0 && lapic_pending((uint8_t)vector))) {
            return 1;
        }
    }
    return 0;
}

/**
 * The controller's message: acknowledge it, pump, and return.
 *
 * The acknowledgement is the pump's first act when the pump was turned by a
 * message, and no part of it when the timer turned it. An event the
 * controller writes between the acknowledgement and the drain is taken by
 * that same drain, which is why the two are one call in the driver.
 *
 * The router ends the interrupt at the APIC after this returns, and
 * reschedules if a reader was woken.
 */
static void usb_msi_handler(isr_params_t *params)
{
    (void)params;

    stamp_entry = pit_now_us();
    usb_messages++;
    (void)usb_pump(0, 1);
    if (msi_drop_after != 0 && usb_messages >= msi_drop_after) {
        /* rach2.msidrop=N: the machine loses its edges from here, and the
         * timer's pump is all that is left. It is how the fallback is seen
         * to work on a machine whose interrupts stop. */
        int n;

        for (n = 0; n < xhci_controllers(); n++) {
            xhci_interrupts_disable(xhci_controller(n));
        }
        klog(KLOG_WARN, "usb", "msidrop: %d messages taken; the controllers were silenced",
             (int)usb_messages);
    }
}

/**
 * Which controller `rach2.hcfail=N` refuses at its bring-up, or -1.
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

int usb_init(void)
{
    int up = 0;
    int n;
    int seen = 0;

    if (xhci_discover() != 0) {
        klog(KLOG_INFO, "usb", "absent: %s", xhci_error(0));
        return -1;
    }
    /*
     * Every controller is brought up on its own. One that refuses is released
     * and left out, and the others still come up: a machine whose second
     * controller is broken keeps the sockets on its first.
     */
    for (n = 0; n < xhci_controllers(); n++) {
        struct xhci_ctrl *hc = xhci_controller(n);
        const struct xhci_caps *caps;
        /* Counted, not indexed: a controller left out shifts the ones after it
         * down and the walk steps back onto this index, so the index is not
         * which controller this is. */
        int refuse = (seen++ == hcfail_at());

        if (refuse || xhci_read_caps(hc) != 0 || xhci_own(hc) != 0 || xhci_halt_reset(hc) != 0 ||
            xhci_scratchpads(hc) != 0 || xhci_start(hc) != 0) {
            if (refuse) {
                klog(KLOG_WARN, "usb", "refused: xhci%d: rach2.hcfail=%d", n, n);
            } else {
                klog(KLOG_WARN, "usb", "refused: xhci%d: %s", n, xhci_error(hc));
            }
            /* Whatever was taken from the firmware goes back. A controller
             * this kernel does not drive is one that should still have
             * whatever its firmware was doing for it. */
            xhci_release(hc);
            /* Out of the table: nothing above may ask a controller that never
             * came up about its ports, and its tables were never taken. The
             * ones after it move up, so the walk steps back onto the one that
             * took this place. */
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

    /* From here the timer drains the controller and steps the devices. This
     * waits for the ports to settle and then for what is plugged in to be
     * enumerated, and does none of the work itself. */
    usb_wait(USB_SETTLE_MS);
    report_ports();
    /* Enumerate what is already plugged in before the first process runs, so
     * a machine that boots with a keyboard has one. Each turn is a step and a
     * millisecond; a device that takes longer than this many is finished by
     * the timer afterwards. */
    {
        uint32_t turn;

        for (turn = 0; turn < USB_ENUMERATE_MS && !usb_core_idle(); turn++) {
            k_udelay(1000);
        }
    }
    klog(KLOG_INFO, "usb", "devices: %d configured", usb_core_count());

    /*
     * The controller's own interrupt, last: everything above ran on the
     * timer's pump, and a message that arrived while the rings were being
     * built would have found a stack that was not ready for it. A machine
     * that refuses stays exactly as it was, which is the kernel before this
     * milestone.
     */
    usb_interrupts_start();
    return 0;
}

/**
 * Take the controller's interrupts as messages, or on its line, or leave it
 * to the timer.
 *
 * `rach2.nomsi` and a processor whose APIC was refused both leave messages
 * off; the controller then raises its interrupts on the PCI line the
 * firmware routed for it, through the 8259, which is the stack every machine
 * had before the local APIC existed and a path eighty times faster than the
 * timer's pump alone for a stick. A controller that names no line is left
 * to the timer. `rach2.msidrop=N` silences the controller after N messages,
 * which is how a machine that loses its edges is staged.
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
    /*
     * A vector per controller. The handler is the one pump and drains them
     * all, so which vector a message arrived on does not matter; what matters
     * is that each controller has one of its own to send, since two
     * controllers sharing a vector cannot both be told to stop.
     */
    for (n = 0; n < xhci_controllers(); n++) {
        struct xhci_ctrl *hc = xhci_controller(n);
        int vector;

        if (lapic_ready() && (vector = isr_alloc_vector()) >= 0) {
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
            /* The handler before the enable: a level line the firmware left
               asserted would otherwise be dispatched to nobody. */
            register_isr_handler((uint8_t)(32 + irq), usb_msi_handler, "xhci");
            if (xhci_interrupts_enable_line(hc) == irq) {
                interrupts_started = 1;
                interrupt_sources[n].line_irq = irq;
                klog(KLOG_INFO, "usb", "interrupts: xhci%d: the line, irq %d on vector %d", n, irq,
                     32 + irq);
                continue;
            }
        }
        klog(KLOG_INFO, "usb", "interrupts: xhci%d: the timer pumps this controller", n);
    }
}

/**
 * Advance the USB stack by one 10 ms timer tick.
 *
 * Count drains with work but no observed pending USB interrupt.
 * A message still in transit can also increase this count.
 */
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
