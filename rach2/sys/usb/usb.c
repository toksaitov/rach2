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
        klog(KLOG_INFO, "usb", "routing: usb2 0x%X of 0x%X, usb3 0x%X of 0x%X, as the firmware left it",
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

/* How long the boot waits for what is already plugged in to be enumerated.
 * Each turn is one step of the state machine and one millisecond, and a device
 * takes a dozen steps, so this is generous for the four devices this kernel
 * holds and short enough not to be noticed. */
#define USB_ENUMERATE_MS 400u

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

/*
 * Whether the stack is up. The timer calls `usb_poll` from the first tick of
 * the boot, and there is nothing to poll until a controller has been found and
 * started.
 */
static int usb_up;

static void usb_interrupts_start(void);

/*
 * The vector the controller's messages arrive on. Vectors 48 to 63 are this
 * kernel's message vectors; the xHCI takes the first of them and the audio
 * controller the second.
 */
#define USB_MSI_VECTOR 48

/* How many messages the controller has sent, how many times the timer's pump
 * found work that a message should have brought, and after how many messages
 * `rach2.msidrop` silences the controller. */
static uint32_t usb_messages;
static uint32_t usb_fallback;
static uint32_t msi_drop_after;

/* Whether the controller was ever put on messages. The fallback is counted
 * against this and not against whether messages are arriving now: a
 * controller silenced by `rach2.msidrop` is exactly the machine whose lost
 * edges are being counted. */
static int msi_started;

/* Whether the controller had an interrupt raised for what the last pump
 * drained. The pump sets it; `usb_poll` reads it. */
static int pump_saw_pending;

/* Whether the pump that is running drained anything. The keystroke's stamps
 * are only worth printing when it did: the numbers are measured from the
 * event, and a pump that took none has no event of its own. */
static int pump_took;

/* Whether a pump was found to have enabled interrupts, so the record is
 * printed once and not every tick. */
static int pump_enabled_interrupts;

/* The stamps of one keystroke's path through this stack, in microseconds:
 * where the handler entered, where the event was taken, and where the key
 * reached the line discipline. `usb_stamp_read` prints them when a reader
 * takes the byte. */
static uint32_t stamp_entry;
static uint32_t stamp_event;
static uint32_t stamp_wake;
static uint32_t wake_event;
static uint32_t wake_entry;
static int stamp_pending;
static int stamp_by_msi;

/* Whether the pump that is running was turned by the controller's message.
 * The keystroke's stamp is taken inside it and takes its answer from here. */
static int in_msi_pump;

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
    uint32_t flags;

    __asm__ __volatile__("pushf\n\t"
                         "pop %0"
                         : "=r"(flags)
                         :
                         : "memory");
    return flags;
}

static int usb_pump(uint32_t elapsed_ms, int from_interrupt)
{
    uint32_t flags;
    int taken;

    flags = kirq_save();
    xhci_advance(elapsed_ms);
    /* The handler's drain acknowledges the interrupt first and in the same
     * call; the timer's has nothing to acknowledge. */
    taken = from_interrupt ? xhci_interrupt_take() : xhci_events();
    pump_took = taken > 0;
    if (taken > 0) {
        stamp_event = pit_now_us();
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
        pump_saw_pending = lapic_pending(USB_MSI_VECTOR);
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
        pump_saw_pending = lapic_pending(USB_MSI_VECTOR);
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
    in_msi_pump = 1;
    (void)usb_pump(0, 1);
    in_msi_pump = 0;
    if (msi_drop_after != 0 && usb_messages >= msi_drop_after) {
        /* rach2.msidrop=N: the machine loses its edges from here, and the
         * timer's pump is all that is left. It is how the fallback is seen
         * to work on a machine whose interrupts stop. */
        xhci_interrupts_disable();
        klog(KLOG_WARN, "usb", "msidrop: %d messages taken; the controller was silenced",
             (int)usb_messages);
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
        /* Whatever was taken from the firmware goes back. A machine that has
         * no USB driver is a machine that should still have whatever its
         * firmware was doing for it. */
        xhci_release();
        return -1;
    }

    caps = xhci_capabilities();
    klog(KLOG_INFO, "usb", "ready: version %x, %d ports, %d slots, %d-byte contexts",
         (int)caps->hciversion, (int)caps->max_ports, (int)caps->max_slots,
         (int)caps->context_size);

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
 * Take the controller's interrupts as messages, or leave it to the timer.
 *
 * `rach2.nomsi` and a processor whose APIC was refused both leave the
 * interrupter off. `rach2.msidrop=N` silences the controller after N
 * messages, which is how a machine that loses its edges is staged.
 */
static void usb_interrupts_start(void)
{
    char value[16];

    if (kcmdline_value("rach2.msidrop", value, (unsigned int)sizeof(value))) {
        uint32_t n = 0;
        int i;

        for (i = 0; i < 6 && value[i] >= '0' && value[i] <= '9'; i++) {
            n = n * 10u + (uint32_t)(value[i] - '0');
        }
        msi_drop_after = n;
    }
    if (!lapic_ready()) {
        klog(KLOG_INFO, "usb", "interrupts: the timer pumps this controller");
        return;
    }
    register_isr_handler(USB_MSI_VECTOR, usb_msi_handler, "xhci");
    if (xhci_interrupts_enable(USB_MSI_VECTOR) != 0) {
        klog(KLOG_INFO, "usb", "interrupts: the timer pumps this controller");
        return;
    }
    msi_started = 1;
    klog(KLOG_INFO, "usb", "interrupts: messages on vector %d", USB_MSI_VECTOR);
}

void usb_poll(void)
{
    if (!usb_up) {
        return;
    }
    pump_saw_pending = 0;
    if (usb_pump(10, 0) > 0 && msi_started && !pump_saw_pending) {
        /*
         * The timer found work that no message was coming for. A message
         * already raised at the controller, or already waiting at the
         * processor, means the timer merely got there first while this tick
         * held the interrupt flag down; that is a race and not a lost edge,
         * and the count is of lost edges.
         */
        usb_fallback++;
    }
}

uint32_t usb_fallback_count(void)
{
    return usb_fallback;
}

/**
 * Note that a keystroke from this stack has reached the line discipline.
 *
 * The third of the four stamps the latency record prints. The fourth is
 * taken where a reader takes the byte, which is the terminal's read.
 */
void usb_stamp_wake(void)
{
    if (!pump_took) {
        /* This pump drained nothing, so the report it is delivering came
         * from an earlier one and there is no event here to measure from.
         * Saying nothing is better than printing a number whose base is
         * several milliseconds old. */
        stamp_pending = 0;
        return;
    }
    stamp_wake = pit_now_us();
    /* The other two are copied here, so that the record printed at the read
     * is one keystroke's throughout. `usb_stamp_read` takes all three back
     * with interrupts off, since a message landing between its own reads
     * would otherwise leave it printing two keystrokes at once. */
    wake_event = stamp_event;
    wake_entry = stamp_entry;
    stamp_by_msi = in_msi_pump;
    stamp_pending = 1;
}

/**
 * A reader took a byte: print the path this keystroke took, once.
 *
 * The numbers are microseconds from the drain that took the event, which is
 * the first moment this stack knows the event exists; the controller stamps
 * nothing, so the time it wrote the event is not a number any of this can
 * report. The handler's entry comes before that drain and so reads as a
 * negative number, and is zero when the timer's pump took the event instead.
 * Then the line discipline, and then the reader, which is the only one of
 * them with anything but this kernel in it: a program that was still writing
 * its last line when the key arrived reads it late, and that time is the
 * program's.
 *
 * A byte that did not come from this stack prints nothing, and neither does
 * one taken so long after the wake that it cannot be the keystroke's. When
 * several keys arrive before a reader takes any of them, the record is the
 * last of them, since there is one set of stamps and not one per byte.
 */
void usb_stamp_read(void)
{
    uint32_t flags;
    uint32_t read_us;
    uint32_t event, entry, wake;
    int by_msi;

    flags = kirq_save();
    if (!stamp_pending) {
        kirq_restore(flags);
        return;
    }
    stamp_pending = 0;
    read_us = pit_now_us();
    event = wake_event;
    entry = wake_entry;
    wake = stamp_wake;
    by_msi = stamp_by_msi;
    kirq_restore(flags);

    /* A byte taken more than two ticks after the keystroke reached the line
     * discipline is not that keystroke's: it is a byte from somewhere else,
     * or one that sat as type-ahead, and the reader's number would say
     * nothing about this path. */
    if (read_us - wake > 20000u) {
        return;
    }
    klog(KLOG_INFO, "usb", "latency: msi %d, entry %d, wake %d, read %d, us from the drain",
         by_msi, by_msi ? (int)(entry - event) : 0, (int)(wake - event), (int)(read_us - event));
}

void usb_stop(void)
{
    usb_up = 0;
    xhci_interrupts_disable();
    xhci_stop();
}


