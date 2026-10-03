/*
 * xhci.c: the USB host controller.
 *
 * The register interface every machine newer than about 2010 presents, and the
 * only one this kernel drives; the notebook's companion EHCI is left alone and
 * its pins are routed here. One controller also handles every speed, so a
 * low-speed keyboard needs no companion controller, no transaction translator
 * and no split transactions, which is most of what a driver for the older
 * interfaces is made of.
 *
 * This file is bring-up and transport: registers, rings, contexts and commands.
 * Descriptors, classes and what a keyboard is belong to `core.c` above it,
 * which touches no register.
 *
 * Bring-up is the only part that waits, and it runs once at boot in thread
 * context. Everything after it is queued and polled, because the completions
 * are drained from the timer's handler and the controller's interrupt, where
 * a wait would stop the clock.
 */

#include "xhci.h"

#include "../klog.h"
#include "../lib/kstring.h"
#include "../mm/kheap.h"
#include "../mm/paging.h"
#include "../arch/x86/pci.h"
#include "../arch/x86/udelay.h"
#include "usb.h"

/*
 * Capability registers, at the start of the aperture. The first word gives the
 * length of this register set in its low byte, which is where the operational
 * registers begin, and the interface version in its high half.
 */
#define XHCI_CAPLENGTH  0x00
#define XHCI_HCSPARAMS1 0x04
#define XHCI_HCSPARAMS2 0x08
#define XHCI_HCCPARAMS1 0x10
#define XHCI_DBOFF      0x14
#define XHCI_RTSOFF     0x18

/* Operational registers, at `CAPLENGTH` from the start of the aperture. */
#define XHCI_USBCMD   0x00
#define XHCI_USBSTS   0x04
#define XHCI_PAGESIZE 0x08
#define XHCI_CONFIG   0x38
#define XHCI_PORTSC   0x400 /* the first port; each port is 16 bytes */

#define USBCMD_RS    0x00000001u
#define USBCMD_HCRST 0x00000002u
#define USBCMD_INTE  0x00000004u

#define USBSTS_HCH  0x00000001u
#define USBSTS_EINT 0x00000008u /* an event interrupt; written as one to clear it */
#define USBSTS_CNR  0x00000800u

/*
 * Extended capabilities, a linked list in the aperture whose head
 * `HCCPARAMS1` names. Each entry's low byte is its identifier and its second
 * byte is the offset to the next one, in dwords, with zero ending the list.
 */
#define XECP_ID_LEGACY 1

/* The legacy support capability: who owns the controller, and the interrupts
 * the firmware was using to keep that ownership useful. */
#define LEGSUP_BIOS_OWNED     0x00010000u
#define LEGSUP_OS_OWNED       0x01000000u
#define LEGCTLSTS             0x04
/* Every system-management interrupt enable in the control register, and the
 * three write-one-to-clear status bits above them. Ownership is taken by
 * clearing the first group and clearing the second, which is what stops the
 * firmware being called back into a controller it no longer owns. */
#define LEGCTLSTS_SMI_ENABLES 0x0000E1FEu
#define LEGCTLSTS_SMI_EVENTS  0xE0000000u

/* How long each wait may take, in microseconds and in polls of that length.
 * The firmware handoff gets a second, which is what other operating systems
 * give it; the halt gets the sixteen milliseconds the specification allows a
 * controller to take; the reset gets a second. */
#define XHCI_POLL_US       1000u
#define XHCI_HANDOFF_POLLS 1000u
#define XHCI_HALT_POLLS    16u
#define XHCI_RESET_POLLS   1000u

/*
 * The pause an Intel controller needs after its reset is asserted, before any
 * register of it may be read again. It is not in the specification; it is in
 * every driver that runs on this hardware, and a driver without it reads
 * rubbish out of a controller that has not finished starting.
 */
#define XHCI_RESET_PAUSE_US 1000u

/*
 * How much of the aperture is mapped. Real controllers publish 64 KiB, and
 * the register sets this driver reaches (the ports at the end of the
 * operational set, the runtime registers, the doorbells) all lie inside it.
 * `xhci_read_caps` refuses a controller that says otherwise rather than
 * writing past the window.
 */
#define XHCI_MAP_SIZE 0x10000u

/* The PCI power-management capability, whose control register holds the power
 * state in its low two bits. A controller left in D3 by the firmware answers
 * its configuration space and not its registers. */
#define PCI_CAP_ID_PM  0x01
#define PCI_PM_CONTROL 0x04

#define XHCI_PCI_PROGIF 0x30

/*
 * Intel routes each USB2 pin to either this controller or the companion EHCI,
 * and each SuperSpeed pin to either the SuperSpeed side of a port or nothing.
 * The two mask registers say which pins may be moved; the two others say where
 * each movable pin currently goes. 8 Series PCH datasheet, xHCI configuration
 * registers.
 */
#define PCI_VENDOR_INTEL 0x8086
#define INTEL_XUSB2PR    0xD0
#define INTEL_XUSB2PRM   0xD4
#define INTEL_USB3PSSEN  0xD8
#define INTEL_USB3PRM    0xDC
/*
 * How many root ports a controller may have. The specification's field is a
 * byte, so this is its whole range; the state below is sized from what the
 * controller says it has, not from a number compiled in.
 */
#define XHCI_MAX_PORTS   255

/* A transfer request block is four words. */
#define TRB_WORDS 4

/* Endpoint numbers run to 15 in each direction, so a device context has 31
 * endpoints and its index runs to 31; a stick picks its own numbers and the
 * ones it picks are its business, not this driver's. */
#define XHCI_MAX_DCI      32
/* An endpoint's ring is one page: 4096 bytes of sixteen-byte blocks. */
#define XHCI_EP_RING_TRBS 256

/* Completions waiting to be reported. A transfer's end is found by the drain
 * of the event ring, so what it ended as is kept until the layer above asks. */
#define XHCI_COMPLETIONS 8

/* The endpoints a transfer record is kept for, and the blocks one holds: a
 * 512 KiB buffer that starts mid-page is 129 pieces, a control transfer
 * three blocks. Two records per endpoint: a stick has a data transfer and
 * a wrapper on one endpoint at most, and the layer above lets a record go
 * at its command's end. */
#define XHCI_TD_DCI     XHCI_MAX_DCI
#define XHCI_TD_TRBS    130
#define XHCI_TD_RECORDS 2

/*
 * One transfer the controller was given: which ring entries it occupies,
 * what each moves, and what the events said about it so far. The record
 * outlives the transfer: it is `retired` once no block of it can be fetched
 * again, which an event naming its last block says, or a stop, a reset with
 * a dequeue, or Disable Slot answered; and a retired record is free for a
 * new submission unless its caller holds it, which a caller that asks about
 * its transfers after their events does until it lets the record go.
 */
struct xhci_td {
    int used;
    int retired;
    int held;      /* the caller asks about it until it lets it go */
    int delivered; /* one completion per transfer */
    int control;   /* setup, data and status blocks; the data block is `idx[1]` */
    int outcome;   /* the first outcome an event gave, a USB_XFER_ value, or -1 */
    int code;
    uint32_t generation;
    uint32_t total;             /* bytes asked, the data stage's for a control transfer */
    uint32_t moved;             /* bytes the events accounted for */
    uint32_t count;             /* blocks, Links not counted */
    uint32_t idx[XHCI_TD_TRBS]; /* each block's ring index */
    uint32_t len[XHCI_TD_TRBS]; /* each block's bytes */
};

struct xhci_slot {
    int used;
    int port;  /* the root port it is under, numbered from zero */
    int speed; /* as the port reported it */
    struct usb_location where;
    /* What the device is when it is a hub: its port count, and for a
     * high-speed hub its translators, which go in its own slot context. */
    int hub_ports;
    int hub_multi_tt;
    int hub_think_time;
    /* The device context the controller reads for this slot, and a ring per
     * endpoint opened on it. Each is one page, taken when the slot or the
     * endpoint is opened and given back when it closes, and each is read by
     * the controller at the physical address recorded beside it. */
    uint8_t *ctx;
    uintptr_t ctx_phys;
    uint32_t *ring[XHCI_MAX_DCI];
    uintptr_t ring_phys[XHCI_MAX_DCI];
    uint32_t enqueue[XHCI_MAX_DCI];
    uint32_t cycle[XHCI_MAX_DCI];
    uint8_t open[XHCI_MAX_DCI];
    /* What the last transfer on each endpoint asked for. The event says what
     * was not moved; what was is the difference, and only this side knows the
     * number it started from. */
    uint32_t asked[XHCI_MAX_DCI];
    /* What the caller called the transfer in flight on each endpoint. It
     * goes back in the completion, so a caller that gave up on one can tell
     * the answer to that from the answer to what it is waiting for now. */
    uint32_t generation[XHCI_MAX_DCI];
    /* Two transfer records per endpoint, allocated on first use. */
    struct xhci_td *td[XHCI_TD_DCI];
    uint32_t max_packet[XHCI_MAX_DCI];
    uint8_t ep_state[XHCI_MAX_DCI];  /* XHCI_EP_RUNNING, _HALTED, _STOPPED or _ERROR */
    uint8_t quiescing[XHCI_MAX_DCI]; /* the core is stopping it: nothing is submitted */
    uint32_t dropped_events;         /* transfer events that named no transfer */
};

/*
 * One controller. Every register window, ring, context and piece of port and
 * command state belongs to the controller it describes, so two controllers on
 * one machine run beside each other: each has its own aperture, its own
 * interrupt, its own rings and its own completions, and nothing one of them
 * does reaches the other.
 *
 * The command ring, event ring, segment table and input context are embedded
 * in this static record and identity-mapped. The context base array,
 * scratchpads, device contexts and endpoint rings are allocated from the heap;
 * their physical addresses are recorded separately from their virtual ones.
 */
struct xhci_ctrl {
    int used;
    int number; /* its place in the table, and the name it is logged by */

    /* Where the legacy-support capability is, and what it held before this
     * kernel touched it. Taking a controller from the firmware and then
     * failing would otherwise leave the firmware's interrupts off and its
     * claim gone, with no driver: on a machine whose firmware is emulating a
     * PS/2 keyboard out of a USB one, that is the keyboard. */
    uint32_t legacy_offset;
    uint32_t legacy_saved_sup;
    uint32_t legacy_saved_ctl;
    int legacy_taken;

    uintptr_t base;
    struct xhci_caps caps;
    const char *err;

    /* Where the controller is on the bus, kept from the walk that found it:
     * the MSI capability is programmed through configuration space, long
     * after. */
    pci_device_t pci;

    /* Whether this controller sends its interrupts as messages, and whether
     * it raises them on its PCI line instead. */
    int msi_on;
    int line_on;

    const struct xhci_io *io;

    /* The scratchpad pages this controller asked for, and the page holding
     * the array of their physical addresses. Both are read by the
     * controller, so each page's physical address is kept beside the
     * address this kernel reaches it by. */
    uint64_t *scratchpad_array;
    uintptr_t scratchpad_array_phys;
    /* The addresses this kernel reaches those pages by, beside the physical
     * ones the array holds for the controller: a page whose only record was
     * its physical address could never be given back. */
    void **scratchpad_page;
    uint32_t scratchpads;

    /* The rings, the context base array and the event ring segment table. */
    uint32_t cmd_ring[XHCI_RING_TRBS * TRB_WORDS] __attribute__((aligned(64)));
    uint32_t event_ring[XHCI_RING_TRBS * TRB_WORDS] __attribute__((aligned(64)));
    /* The context base array, one page: the controller reads it by its
     * physical address, and a page holds 512 entries, more than the 255
     * slots the standard's field can name. */
    uint64_t *dcbaa;
    uintptr_t dcbaa_phys;
    uint32_t nslots; /* the slots this controller was told to enable */
    uint32_t erst[4] __attribute__((aligned(64)));

    uint32_t cmd_enqueue; /* the block this driver writes next */
    uint32_t cmd_cycle;   /* the cycle value it writes */
    uint32_t event_index; /* the block it reads next */
    uint32_t event_cycle; /* the cycle value it expects there */

    /* Whether the controller is running. Nothing may be read from its event
     * ring before it is, and the timer calls into this driver from the first
     * tick of the boot, long before there is a controller to ask. */
    int running;

    /* The command in flight, if any, and what it ended as. */
    int command_running;
    int command_finished;
    int command_aborting;
    int command_stop_seen;
    struct usb_command_result command_result;
    uint32_t command_deadline;
    uint32_t command_trb;
    uint32_t command_type;

    /* This driver's own clock, in milliseconds, and the per-port state that
     * is counted against it. */
    uint32_t now_ms;
    /* One entry per root port the controller says it has, taken at
     * bring-up. */
    uint32_t *port_deadline;
    uint32_t *port_connect_at;
    uint8_t *port_connected;
    uint8_t *port_changed;

    struct usb_completion completions[XHCI_COMPLETIONS];
    uint32_t completion_head;
    uint32_t completion_tail;

    /* The contexts and the rings. The input context is one, used for one
     * command at a time, which is all this driver issues. Each device has its
     * own output context and a ring for each endpoint it opens. */
    uint8_t input_ctx[(XHCI_MAX_DCI + 1) * 64] __attribute__((aligned(64)));
    struct xhci_slot *slots;

    /* The port and speed a slot is being taken for. The controller decides
     * which slot that will be and does not say so until the command answers,
     * so what the slot will describe is kept here in the meantime. */
    int pending_port;
    int pending_speed;
    struct usb_location pending_where;
};

static struct xhci_ctrl ctrls[XHCI_MAX_CONTROLLERS];
static int nctrls;

/*
 * A compiler barrier. The controller reads what this driver writes and writes
 * what it reads, so the order of a fill and the doorbell that publishes it
 * matters. x86 gives the store ordering; this stops the compiler from
 * reordering or eliding what it cannot see another party doing.
 */
#ifndef xhci_barrier
#define xhci_barrier() __asm__ __volatile__("" : : : "memory")
#endif

static uint32_t xhci_mmio_r32(struct xhci_ctrl *hc, uint32_t offset)
{
    return *(volatile uint32_t *)(hc->base + offset);
}

static void xhci_mmio_w32(struct xhci_ctrl *hc, uint32_t offset, uint32_t value)
{
    *(volatile uint32_t *)(hc->base + offset) = value;
}

static const struct xhci_io xhci_mmio_io = {xhci_mmio_r32, xhci_mmio_w32, k_udelay};

/*
 * The register accessors a controller found from now on will use. A host test
 * supplies its model before discovery runs, and each controller keeps the
 * pointer it was created with, so two controllers may be driven through two
 * different models.
 */
static const struct xhci_io *xhci_default_io = &xhci_mmio_io;

void xhci_set_io(const struct xhci_io *ops)
{
    xhci_default_io = ops ? ops : &xhci_mmio_io;
}

static uint32_t r32(struct xhci_ctrl *hc, uint32_t offset)
{
    return hc->io->r32(hc, offset);
}

static void w32(struct xhci_ctrl *hc, uint32_t offset, uint32_t value)
{
    hc->io->w32(hc, offset, value);
}

/** An operational register, which lives past the capability registers. */
static uint32_t op_r32(struct xhci_ctrl *hc, uint32_t offset)
{
    return r32(hc, hc->caps.caplength + offset);
}

static void op_w32(struct xhci_ctrl *hc, uint32_t offset, uint32_t value)
{
    w32(hc, hc->caps.caplength + offset, value);
}

const struct xhci_caps *xhci_capabilities(struct xhci_ctrl *hc)
{
    return &hc->caps;
}

static void slot_pages_free(struct xhci_slot *s);
static void ctrl_free_tables(struct xhci_ctrl *hc);
static struct xhci_td *td_at(struct xhci_slot *s, uint32_t dci, uint32_t at, uint32_t *k);

/* What the last discovery had to say when it found no controller at all.
 * Before there is a record there is nowhere else to put it. */
static const char *discover_err = "not tried";

const char *xhci_error(struct xhci_ctrl *hc)
{
    return hc ? hc->err : discover_err;
}

/**
 * Put a function in D0, so that its registers answer.
 *
 * A controller the firmware parked in a low power state responds to
 * configuration space and returns all ones from its aperture, which reads as a
 * controller that is present and broken. Writing the state costs one access
 * and removes that whole class of confusion.
 */
static void power_up(const pci_device_t *dev)
{
    uint8_t cap = pci_find_capability(dev->bus, dev->device, dev->function, PCI_CAP_ID_PM);

    if (cap != 0) {
        uint16_t pmcsr = pci_read_config16(dev->bus, dev->device, dev->function,
                                           (uint8_t)(cap + PCI_PM_CONTROL));
        if ((pmcsr & 0x3u) != 0) {
            pci_write_config16(dev->bus, dev->device, dev->function,
                               (uint8_t)(cap + PCI_PM_CONTROL), (uint16_t)(pmcsr & ~0x3u));
            /* A function leaving D3 has 10 ms before it must answer. */
            k_udelay(10000);
            pmcsr = pci_read_config16(dev->bus, dev->device, dev->function,
                                      (uint8_t)(cap + PCI_PM_CONTROL));
            if ((pmcsr & 0x3u) != 0) {
                klog(KLOG_WARN, "usb", "power: the controller stayed in state %d",
                     (int)(pmcsr & 0x3u));
            }
        }
    }
}

/**
 * Take the pins the chipset will give.
 *
 * A firmware leaves some USB2 pins on the companion EHCI so that an operating
 * system with no xHCI driver still has working ports. This kernel has no EHCI
 * driver, so a pin left there is a socket nothing can reach: on the notebook
 * the firmware gave this controller two of nine, and the other seven were the
 * two sockets a person plugs into.
 *
 * So the movable pins are moved, which is what every other operating system on
 * this hardware does at the same point. Only the pins the mask allows, and the
 * mask is read rather than assumed: a pin the chipset will not move is one the
 * write has no effect on. The routing is the firmware's again after the
 * next power cycle, so this runs on every boot and a machine on which it goes
 * wrong is one power cycle from the way it was.
 */
static void take_ports(const pci_device_t *dev)
{
    uint32_t usb2_mask;
    uint32_t usb2_before;
    uint32_t usb3_mask;
    uint32_t usb3_before;

    if (dev->vendor_id != PCI_VENDOR_INTEL) {
        return; /* another maker's controller routes its pins another way */
    }

    usb2_before = pci_read_config32(dev->bus, dev->device, dev->function, INTEL_XUSB2PR);
    usb2_mask = pci_read_config32(dev->bus, dev->device, dev->function, INTEL_XUSB2PRM);
    usb3_before = pci_read_config32(dev->bus, dev->device, dev->function, INTEL_USB3PSSEN);
    usb3_mask = pci_read_config32(dev->bus, dev->device, dev->function, INTEL_USB3PRM);

    if ((usb2_before & usb2_mask) != usb2_mask) {
        pci_write_config32(dev->bus, dev->device, dev->function, INTEL_XUSB2PR,
                           usb2_before | usb2_mask);
    }
    if ((usb3_before & usb3_mask) != usb3_mask) {
        pci_write_config32(dev->bus, dev->device, dev->function, INTEL_USB3PSSEN,
                           usb3_before | usb3_mask);
    }

    klog(KLOG_INFO, "usb", "routing: usb2 0x%X of 0x%X, usb3 0x%X of 0x%X, taken",
         pci_read_config32(dev->bus, dev->device, dev->function, INTEL_XUSB2PR), usb2_mask,
         pci_read_config32(dev->bus, dev->device, dev->function, INTEL_USB3PSSEN), usb3_mask);
}

/**
 * Take one controller found on the bus into the record `hc`: put it in D0,
 * route the Intel pins to it, enable its memory space and bus mastering, and
 * map its aperture into the kernel MMIO region, whose virtual address is used
 * for register access from then on.
 *
 * A 64-bit aperture is legal and common, and its upper half must be zero
 * here: this kernel addresses 32 bits, and taking the low half of a base
 * above 4 GiB would point the driver at some other device's registers.
 *
 * @return 0, or -1 with `hc->err` saying what stopped it.
 */
static int discover_one(struct xhci_ctrl *hc, pci_device_t *dev)
{
    uint32_t base;
    uintptr_t mapped;

    if (!pci_bar_is_memory(dev->bar[0])) {
        hc->err = "the controller's aperture is not a memory window";
        return -1;
    }
    if (pci_bar_is_64bit(dev->bar[0]) && dev->bar[1] != 0) {
        hc->err = "the controller's aperture is above 4 GiB";
        return -1;
    }
    base = pci_bar_address(dev->bar[0]);
    if (base == 0) {
        hc->err = "the controller's aperture was never programmed";
        return -1;
    }

    hc->pci = *dev;
    power_up(dev);
    take_ports(dev);
    pci_enable_memory_space(dev);
    pci_enable_bus_master(dev);

    mapped = map_mmio(base, XHCI_MAP_SIZE);
    if (mapped == 0) {
        hc->err = "the controller's aperture could not be mapped";
        return -1;
    }
    hc->base = mapped;

    klog(KLOG_INFO, "usb", "found: xhci%d: %x:%x at %d:%d.%d, aperture 0x%X", hc->number,
         (int)dev->vendor_id, (int)dev->device_id, (int)dev->bus, (int)dev->device,
         (int)dev->function, base);
    hc->err = "none";
    return 0;
}

/**
 * Walk the bus and take a record for every xHCI controller on it.
 *
 * Up to XHCI_MAX_CONTROLLERS controllers are taken, in the order the walk
 * finds them, and each gets its own record. One for which `discover_one`
 * fails is left out of the table and says why; the others are unaffected,
 * which is the point of a record per controller. A controller of another
 * interface, or one past the table's end, is named and not driven, and the
 * walk goes on so that the record says everything the machine has.
 *
 * @return 0 when at least one controller was taken, -1 otherwise.
 */
int xhci_discover(void)
{
    pci_device_t dev;
    pci_device_t skip;
    pci_device_t *skip_dev = 0;
    int refused = 0;
    int said_full = 0;

    nctrls = 0;
    while (pci_find_device_skip(PCI_CLASS_SERIAL_BUS, PCI_SUBCLASS_USB, skip_dev, &dev)) {
        skip = dev;
        skip_dev = &skip;
        if (dev.prog_if != XHCI_PCI_PROGIF) {
            klog(KLOG_INFO, "usb", "unsupported: %s at %02x:%02x.%d",
                 usb_interface_name(dev.prog_if), (int)dev.bus, (int)dev.device, (int)dev.function);
            continue;
        }
        if (nctrls == XHCI_MAX_CONTROLLERS) {
            if (!said_full) {
                said_full = 1;
                klog(KLOG_WARN, "usb", "refused: more than %d controllers; the rest are not driven",
                     XHCI_MAX_CONTROLLERS);
            }
            klog(KLOG_INFO, "usb", "not driven: an xHCI controller at %02x:%02x.%d", (int)dev.bus,
                 (int)dev.device, (int)dev.function);
            continue;
        }
        {
            struct xhci_ctrl *hc = &ctrls[nctrls];

            /* A record from an earlier discovery frees its tables first. */
            ctrl_free_tables(hc);
            k_memset(hc, 0, sizeof(*hc));
            hc->number = nctrls;
            hc->io = xhci_default_io;
            hc->err = "not tried";
            if (discover_one(hc, &dev) != 0) {
                klog(KLOG_WARN, "usb", "refused: a controller at %02x:%02x.%d: %s", (int)dev.bus,
                     (int)dev.device, (int)dev.function, hc->err);
                refused++;
                continue;
            }
            hc->used = 1;
            nctrls++;
        }
    }
    if (nctrls == 0) {
        discover_err = refused ? "every xHCI controller on the bus was refused"
                               : "no xHCI controller on the bus";
        return -1;
    }
    discover_err = "none";
    return 0;
}

int xhci_controllers(void)
{
    return nctrls;
}

/**
 * Drop a controller that did not come up, freeing whatever tables it took.
 *
 * Nothing else may look at it: on a controller that failed before its tables
 * were sized, a port's state would be read through a null. Discovery put
 * it in the table before its bring-up was tried, and this is what takes it
 * out again. The controllers after it move up and are renumbered, which is
 * safe because bring-up goes in table order, so none of them has been
 * brought up and nothing refers to them yet.
 */
void xhci_forget(struct xhci_ctrl *hc)
{
    int i;
    int at = -1;

    for (i = 0; i < nctrls; i++) {
        if (&ctrls[i] == hc) {
            at = i;
            break;
        }
    }
    if (at < 0) {
        return;
    }
    ctrl_free_tables(&ctrls[at]);
    for (i = at; i + 1 < nctrls; i++) {
        ctrls[i] = ctrls[i + 1];
        ctrls[i].number = i;
    }
    nctrls--;
}

struct xhci_ctrl *xhci_controller(int n)
{
    return n >= 0 && n < nctrls ? &ctrls[n] : 0;
}

int xhci_number(const struct xhci_ctrl *hc)
{
    return hc ? hc->number : -1;
}

/**
 * Take the controller from the firmware through the legacy-support
 * capability, when it has one.
 *
 * The capability list is the controller's, so where it leads is checked
 * rather than trusted. A next-pointer only ever moves forward, and every step
 * that continues moves at least four bytes, so the mapped window is what ends
 * the walk: a list that reaches the end of it is refused rather than followed
 * into whatever the aperture is next to.
 *
 * The OS-owned bit is set, and a firmware that owns the controller has a
 * second to clear the BIOS-owned bit. Ownership is both bits, the firmware's
 * claim gone and this kernel's in place: a controller that dropped the
 * request keeps the second clear, and a driver that looked only at the first
 * would drive a controller it does not own. The system-management interrupts
 * are then turned off and their status cleared, even on a controller the
 * firmware did not claim, since a firmware that released ownership may have
 * left its callbacks enabled. What the capability had before is kept for
 * `xhci_release`.
 *
 * @return 0, or -1 with `hc->err` saying what stopped it.
 */
int xhci_own(struct xhci_ctrl *hc)
{
    uint32_t offset = hc->caps.xecp;
    uint32_t polls;

    while (offset != 0) {
        uint32_t entry;
        uint32_t next;

        if (offset + 8u > XHCI_MAP_SIZE) {
            hc->err = "the controller's capability list leaves its aperture";
            return -1;
        }
        entry = r32(hc, offset);
        next = (entry >> 8) & 0xFFu;

        if ((entry & 0xFFu) == XECP_ID_LEGACY) {
            uint32_t control;

            hc->legacy_offset = offset;
            hc->legacy_saved_sup = entry;
            hc->legacy_saved_ctl = r32(hc, offset + LEGCTLSTS);
            klog(KLOG_DEBUG, "usb", "handoff: legsup 0x%X legctl 0x%X before", hc->legacy_saved_sup,
                 hc->legacy_saved_ctl);

            if (!(entry & LEGSUP_BIOS_OWNED)) {
                w32(hc, offset, entry | LEGSUP_OS_OWNED);
            } else {
                w32(hc, offset, entry | LEGSUP_OS_OWNED);
                for (polls = 0; polls < XHCI_HANDOFF_POLLS; polls++) {
                    if (!(r32(hc, offset) & LEGSUP_BIOS_OWNED)) {
                        break;
                    }
                    hc->io->udelay(XHCI_POLL_US);
                }
                if (r32(hc, offset) & LEGSUP_BIOS_OWNED) {
                    hc->err = "the firmware never gave up the controller";
                    return -1;
                }
            }
            if (!(r32(hc, offset) & LEGSUP_OS_OWNED)) {
                hc->err = "the controller did not take this kernel's claim";
                return -1;
            }

            control = r32(hc, offset + LEGCTLSTS);
            control &= ~LEGCTLSTS_SMI_ENABLES;
            control |= LEGCTLSTS_SMI_EVENTS;
            w32(hc, offset + LEGCTLSTS, control);
            hc->legacy_taken = 1;
            klog(KLOG_DEBUG, "usb", "handoff: legsup 0x%X legctl 0x%X after", r32(hc, offset),
                 r32(hc, offset + LEGCTLSTS));
            hc->err = "none";
            return 0;
        }

        if (next == 0) {
            break;
        }
        offset += next * 4;
    }

    /* No legacy capability: the controller was never the firmware's to hold. */
    hc->err = "none";
    return 0;
}

/**
 * Halt the controller and reset it.
 *
 * It is first waited for while its status says it is not ready: such a
 * controller may have no register written but its status, and one coming out
 * of D3 or out of a reset the firmware started says so for milliseconds.
 * Then it is halted, within the sixteen milliseconds the specification
 * allows, reset, given the pause an Intel controller needs, and waited for
 * until the reset ends and it is ready.
 *
 * @return 0, or -1 with `hc->err` saying what stopped it.
 */
int xhci_halt_reset(struct xhci_ctrl *hc)
{
    uint32_t polls;
    uint32_t cmd;

    for (polls = 0; polls < XHCI_RESET_POLLS; polls++) {
        if (!(op_r32(hc, XHCI_USBSTS) & USBSTS_CNR)) {
            break;
        }
        hc->io->udelay(XHCI_POLL_US);
    }
    if (op_r32(hc, XHCI_USBSTS) & USBSTS_CNR) {
        hc->err = "the controller was not ready to be halted";
        return -1;
    }

    cmd = op_r32(hc, XHCI_USBCMD);
    op_w32(hc, XHCI_USBCMD, cmd & ~(USBCMD_RS | USBCMD_INTE));
    for (polls = 0; polls < XHCI_HALT_POLLS; polls++) {
        if (op_r32(hc, XHCI_USBSTS) & USBSTS_HCH) {
            break;
        }
        hc->io->udelay(XHCI_POLL_US);
    }
    if (!(op_r32(hc, XHCI_USBSTS) & USBSTS_HCH)) {
        hc->err = "the controller never halted";
        return -1;
    }

    op_w32(hc, XHCI_USBCMD, op_r32(hc, XHCI_USBCMD) | USBCMD_HCRST);
    hc->io->udelay(XHCI_RESET_PAUSE_US);
    for (polls = 0; polls < XHCI_RESET_POLLS; polls++) {
        if (!(op_r32(hc, XHCI_USBCMD) & USBCMD_HCRST) && !(op_r32(hc, XHCI_USBSTS) & USBSTS_CNR)) {
            break;
        }
        hc->io->udelay(XHCI_POLL_US);
    }
    if (op_r32(hc, XHCI_USBCMD) & USBCMD_HCRST) {
        hc->err = "the controller's reset never completed";
        return -1;
    }
    if (op_r32(hc, XHCI_USBSTS) & USBSTS_CNR) {
        hc->err = "the controller never became ready";
        return -1;
    }

    hc->err = "none";
    return 0;
}

/**
 * Give back the tables sized from a controller's capability registers, and
 * every slot's own pages with the slot table.
 *
 * Reading those registers again sizes the tables again, so what was taken for
 * the old sizes goes back first; a record reused by a later discovery, and a
 * controller forgotten, give theirs back here too. A slot's own pages are
 * otherwise taken when the slot is addressed and given back when it closes.
 */
static void ctrl_free_tables(struct xhci_ctrl *hc)
{
    uint32_t i;

    if (hc->slots) {
        for (i = 0; i <= hc->nslots; i++) {
            slot_pages_free(&hc->slots[i]);
        }
        kfree(hc->slots);
        hc->slots = 0;
    }
    if (hc->dcbaa) {
        kfree_pages(hc->dcbaa, 1);
        hc->dcbaa = 0;
        hc->dcbaa_phys = 0;
    }
    if (hc->scratchpad_page) {
        for (i = 0; i < hc->scratchpads; i++) {
            if (hc->scratchpad_page[i]) {
                kfree_pages(hc->scratchpad_page[i], 1);
            }
        }
        kfree(hc->scratchpad_page);
        hc->scratchpad_page = 0;
    }
    if (hc->scratchpad_array) {
        kfree_pages(hc->scratchpad_array, 1);
        hc->scratchpad_array = 0;
        hc->scratchpad_array_phys = 0;
    }
    hc->scratchpads = 0;
    kfree(hc->port_deadline);
    kfree(hc->port_connect_at);
    kfree(hc->port_connected);
    kfree(hc->port_changed);
    hc->port_deadline = 0;
    hc->port_connect_at = 0;
    hc->port_connected = 0;
    hc->port_changed = 0;
    hc->nslots = 0;
}

/**
 * Read and validate the controller's register ranges before allocating its
 * tables. Every accessed register must fit within the mapped aperture.
 *
 * The tables are sized by the controller's own counts, after any tables an
 * earlier read took are given back: one slot record per slot for this driver
 * and one page for the context base array the controller reads, and one
 * entry per root port for the port state. The raw capability words are
 * recorded as well as what was read out of them, since a session at a
 * machine this kernel has never met takes the log away and needs them to
 * compare with the datasheet.
 *
 * @return 0, or -1 with `hc->err` saying what stopped it.
 */
int xhci_read_caps(struct xhci_ctrl *hc)
{
    uint32_t word;
    uint32_t ports_end;
    uint32_t doorbells_bytes;
    uint32_t runtime_bytes;

    word = r32(hc, XHCI_CAPLENGTH);
    hc->caps.caplength = word & 0xFFu;
    hc->caps.hciversion = (word >> 16) & 0xFFFFu;

    word = r32(hc, XHCI_HCSPARAMS1);
    hc->caps.max_slots = word & 0xFFu;
    hc->caps.max_intrs = (word >> 8) & 0x7FFu;
    hc->caps.max_ports = (word >> 24) & 0xFFu;

    /* The scratchpad count's high five bits are at bit 21, its low at 27. */
    word = r32(hc, XHCI_HCSPARAMS2);
    hc->caps.erst_max = (word >> 4) & 0xFu;
    hc->caps.scratchpads = (((word >> 21) & 0x1Fu) << 5) | ((word >> 27) & 0x1Fu);

    word = r32(hc, XHCI_HCCPARAMS1);
    hc->caps.ac64 = (int)(word & 0x1u);
    hc->caps.context_size = (word & 0x4u) ? 64u : 32u;
    hc->caps.xecp = ((word >> 16) & 0xFFFFu) * 4u;

    hc->caps.dboff = r32(hc, XHCI_DBOFF) & ~0x3u;
    hc->caps.rtsoff = r32(hc, XHCI_RTSOFF) & ~0x1Fu;

    if (hc->caps.caplength < 0x20u || hc->caps.max_ports == 0 || hc->caps.max_slots == 0 ||
        hc->caps.max_intrs == 0) {
        hc->err = "the controller's capability registers describe no controller";
        return -1;
    }

    ports_end = hc->caps.caplength + XHCI_PORTSC + hc->caps.max_ports * 0x10u;
    doorbells_bytes = (hc->caps.max_slots + 1u) * 4u;
    runtime_bytes = 0x20u + hc->caps.max_intrs * 0x20u;
    if (ports_end > XHCI_MAP_SIZE || hc->caps.dboff == 0 || hc->caps.rtsoff == 0 ||
        hc->caps.dboff > XHCI_MAP_SIZE || hc->caps.rtsoff > XHCI_MAP_SIZE ||
        doorbells_bytes > XHCI_MAP_SIZE - hc->caps.dboff ||
        runtime_bytes > XHCI_MAP_SIZE - hc->caps.rtsoff) {
        hc->err = "the controller's registers do not fit the mapped aperture";
        return -1;
    }

    if (hc->caps.max_ports > XHCI_MAX_PORTS) {
        hc->err = "the controller says it has a number of ports the standard does not allow";
        return -1;
    }
    ctrl_free_tables(hc);

    if (hc->caps.max_slots == 0 || hc->caps.max_slots > XHCI_SLOTS) {
        hc->err = "the controller says it has a number of slots the standard does not allow";
        return -1;
    }
    hc->nslots = hc->caps.max_slots;
    hc->slots = (struct xhci_slot *)kzalloc((hc->nslots + 1) * sizeof(struct xhci_slot));
    hc->dcbaa = (uint64_t *)kalloc_pages(1, &hc->dcbaa_phys);
    if (!hc->slots || !hc->dcbaa) {
        hc->err = "the slot table could not be allocated";
        return -1;
    }
    k_memset(hc->dcbaa, 0, 4096);

    hc->port_deadline = (uint32_t *)kzalloc(hc->caps.max_ports * sizeof(uint32_t));
    hc->port_connect_at = (uint32_t *)kzalloc(hc->caps.max_ports * sizeof(uint32_t));
    hc->port_connected = (uint8_t *)kzalloc(hc->caps.max_ports);
    hc->port_changed = (uint8_t *)kzalloc(hc->caps.max_ports);
    if (!hc->port_deadline || !hc->port_connect_at || !hc->port_connected || !hc->port_changed) {
        hc->err = "the port state could not be allocated";
        return -1;
    }

    hc->caps.pagesize = op_r32(hc, XHCI_PAGESIZE) & 0xFFFFu;
    klog(KLOG_DEBUG, "usb", "caps: hcs1 0x%X hcs2 0x%X hcc1 0x%X pagesize 0x%X",
         r32(hc, XHCI_HCSPARAMS1), r32(hc, XHCI_HCSPARAMS2), r32(hc, XHCI_HCCPARAMS1),
         hc->caps.pagesize);
    klog(KLOG_DEBUG, "usb", "caps: scratchpads %d, contexts %d bytes, 64-bit %d",
         (int)hc->caps.scratchpads, (int)hc->caps.context_size, hc->caps.ac64);
    if (!(hc->caps.pagesize & 0x1u)) {
        hc->err = "the controller does not take 4 KiB pages";
        return -1;
    }

    hc->err = "none";
    return 0;
}

/**
 * Give the controller the scratchpad pages it asked for.
 *
 * The count is the controller's own, from its capability registers, and the
 * pages come from the heap: a controller that wants a hundred gets a hundred
 * rather than being refused by a number compiled into this driver. The array
 * of their addresses is one page, which holds 512 of them, and a controller
 * asking for more than that is refused by name.
 *
 * Both the array and the pages are read by the controller at their physical
 * addresses, so each is taken a page at a time and its physical address
 * recorded: the heap's pages are contiguous where this kernel reaches them
 * and scattered where the controller does.
 */
int xhci_scratchpads(struct xhci_ctrl *hc)
{
    uint32_t i;
    uintptr_t phys;

    hc->scratchpads = hc->caps.scratchpads;
    if (hc->scratchpads == 0) {
        hc->err = "none";
        return 0;
    }
    if (hc->scratchpads > XHCI_MAX_SCRATCHPADS) {
        klog(KLOG_ERR, "usb", "scratchpad: the controller requires %d pages and the array holds %d",
             (int)hc->scratchpads, (int)XHCI_MAX_SCRATCHPADS);
        hc->err = "the controller requires more scratchpad pages than the array holds";
        return -1;
    }
    hc->scratchpad_array = (uint64_t *)kalloc_pages(1, &hc->scratchpad_array_phys);
    hc->scratchpad_page = (void **)kzalloc(hc->scratchpads * sizeof(void *));
    if (!hc->scratchpad_array || !hc->scratchpad_page) {
        hc->err = "the scratchpad array could not be allocated";
        return -1;
    }
    k_memset(hc->scratchpad_array, 0, 4096);
    for (i = 0; i < hc->scratchpads; i++) {
        void *page = kalloc_pages(1, &phys);

        if (!page) {
            hc->err = "a scratchpad page could not be allocated";
            return -1;
        }
        k_memset(page, 0, 4096);
        hc->scratchpad_page[i] = page;
        hc->scratchpad_array[i] = (uint64_t)phys;
    }

    hc->err = "none";
    return 0;
}

/* the rings */

/*
 * A transfer request block is four words: two of parameter, one of status, and
 * one of control whose lowest bit is the cycle bit and whose bits 15 to 10 are
 * the type. The cycle bit is how the controller and this driver tell whose
 * turn a block is: each writes blocks with the current cycle value and toggles
 * that value when it wraps past the end of its ring, so a block the other side
 * has not reached yet always reads as the wrong cycle.
 */
#define TRB_CYCLE      0x1u
#define TRB_TYPE_SHIFT 10
#define TRB_TYPE_MASK  0x3Fu
#define TRB_TOGGLE     0x2u /* on a Link block: toggle the cycle here */
#define TRB_SLOT_SHIFT 24

#define TRB_TYPE_LINK             6
#define TRB_TYPE_NOOP_COMMAND     23
#define TRB_TYPE_TRANSFER_EVENT   32
#define TRB_TYPE_COMMAND_COMPLETE 33
#define TRB_TYPE_PORT_CHANGE      34

/* The completion code a controller reports in the third word of an event. One
 * is success; everything else is a refusal this driver hands upwards. */
#define TRB_COMPLETION_SHIFT   24
#define TRB_COMPLETION_SUCCESS 1

/* Operational registers of the command ring and the context base array. */
#define XHCI_CRCR   0x18
#define XHCI_DCBAAP 0x30

#define CRCR_RCS 0x1u /* the ring cycle state the controller should start at */
#define CRCR_CA  0x4u /* abort the command in flight */
#define CRCR_CRR 0x8u /* a command is running */

/* Runtime registers, at `RTSOFF`. The first interrupter is 32 bytes in. */
#define XHCI_IR0     0x20
#define IR_IMAN      0x00
#define IR_IMOD      0x04
#define IR_ERSTSZ    0x08
#define IR_ERSTBA    0x10
#define IR_ERDP      0x18
#define ERDP_HANDLER 0x8u /* write it back to say the events were taken */

#define IMAN_IP 0x1u /* an interrupt is pending; written as one to clear it */
#define IMAN_IE 0x2u /* the interrupter may raise one */

/* Port status and control, one per port, sixteen bytes apart. */
#define PORTSC_CCS         0x00000001u /* a device is connected */
#define PORTSC_PED         0x00000002u /* the port is enabled; write-one-to-CLEAR */
#define PORTSC_PR          0x00000010u /* reset the port */
#define PORTSC_PP          0x00000200u /* the port has power */
#define PORTSC_SPEED_SHIFT 10
#define PORTSC_SPEED_MASK  0xFu
/* Every change bit, all of them write-one-to-clear. A write that does not mask
 * these off clears whichever of them happened to be set, and the connect this
 * driver had not noticed yet is gone. */
#define PORTSC_CHANGES     0x00FE0000u
#define PORTSC_CSC         0x00020000u
#define PORTSC_PRC         0x00200000u

/* How long a port is given to settle after a connect, and to finish a reset.
 * The debounce is the interval the USB specification requires between seeing a
 * connect and driving a reset. */
#define PORT_DEBOUNCE_MS 100u
#define PORT_RESET_MS    500u

/* How long a command may take before the ring is taken back from the
 * controller. Nothing this driver issues is slow; a command still running
 * after this is a controller that has stopped answering. */
#define COMMAND_MS 1000u

/**
 * Whether a moment has passed, in a way that survives the clock wrapping.
 *
 * The clock is milliseconds in 32 bits, which wraps after seven weeks. A plain
 * comparison against a deadline computed before the wrap says the deadline has
 * not arrived for another seven weeks; the difference, read as signed, says
 * what it should.
 */
static int past(struct xhci_ctrl *hc, uint32_t deadline)
{
    return (int32_t)(hc->now_ms - deadline) >= 0;
}

/* devices, endpoints and transfers */

/*
 * A device's context is an array of entries indexed by the endpoint's device
 * context index: one for the slot, then one per endpoint direction. Endpoint
 * numbered N is index 2N for the direction out and 2N plus one for in.
 * Endpoint numbers 1 through 15 use indices 2 through 31. Endpoint zero uses
 * index 1.
 */

/* Command and transfer block types. */
#define TRB_TYPE_NORMAL         1
#define TRB_TYPE_SETUP          2
#define TRB_TYPE_DATA           3
#define TRB_TYPE_STATUS         4
#define TRB_TYPE_ENABLE_SLOT    9
#define TRB_TYPE_DISABLE_SLOT   10
#define TRB_TYPE_ADDRESS_DEVICE 11
#define TRB_TYPE_CONFIG_EP      12
#define TRB_TYPE_EVALUATE_CTX   13
#define TRB_TYPE_RESET_EP       14
#define TRB_TYPE_STOP_EP        15
#define TRB_TYPE_SET_TR_DEQUEUE 16

#define TRB_IOC           0x20u /* raise an event when this block is done */
#define TRB_IDT           0x40u /* the parameter is the data, not a pointer to it */
#define TRB_ISP           0x4u  /* an event for a short packet on this block */
#define TRB_CHAIN         0x10u /* the next block is part of the same transfer */
#define TRB_TD_SIZE_SHIFT 17
#define TRB_DIR_IN        0x10000u
#define TRB_TRT_NO_DATA   0u
#define TRB_TRT_OUT       (2u << 16)
#define TRB_TRT_IN        (3u << 16)

/* Completion codes this driver tells apart from every other refusal. */
#define TRB_COMPLETION_SHORT         13
#define TRB_COMPLETION_STALL         6
#define TRB_COMPLETION_TXN_ERROR     4
#define TRB_COMPLETION_BABBLE        3
#define TRB_COMPLETION_SPLIT_ERROR   17
#define TRB_COMPLETION_CTX_STATE     19
#define TRB_COMPLETION_RING_STOPPED  24
#define TRB_COMPLETION_STOPPED       26
#define TRB_COMPLETION_STOPPED_LEN   27
#define TRB_COMPLETION_STOPPED_SHORT 28

static void command_abort(struct xhci_ctrl *hc);

/**
 * Advance the controller's clock by `milliseconds`, and abort a command past
 * its deadline whether or not anyone still waits for it.
 *
 * The device that issued the command may have been given up on at its own,
 * shorter deadline, and a command left running keeps the ring from every
 * device after it; an Address Device a soldered port never answers would
 * otherwise cost the machine every USB device.
 */
void xhci_advance(struct xhci_ctrl *hc, uint32_t milliseconds)
{
    hc->now_ms += milliseconds;
    if (hc->command_running && !hc->command_aborting && past(hc, hc->command_deadline)) {
        command_abort(hc);
    }
}

/** A word of a block in a ring. */
static uint32_t *trb_at(uint32_t *ring, uint32_t index)
{
    return &ring[index * TRB_WORDS];
}

/** A TRB's address as the controller takes it: the command and event rings
 * are in the identity-mapped window, so the virtual address is the physical
 * one. */
static uint32_t trb_addr(uint32_t *trb)
{
    return (uint32_t)(uintptr_t)trb;
}

/**
 * The address of a port's status and control register.
 *
 * Ports are numbered from one in every xHCI document, and from zero in every
 * loop here, so the conversion happens in one place.
 */
static uint32_t portsc_offset(struct xhci_ctrl *hc, int port)
{
    return hc->caps.caplength + XHCI_PORTSC + (uint32_t)port * 0x10u;
}

/**
 * Write a port register without disturbing what the write would otherwise
 * clear.
 *
 * Half the bits in this register are write-one-to-clear, including the one
 * that enables the port, so a driver that reads the register, sets its bit and
 * writes the result back disables the port and throws away every change it had
 * not noticed. What is written here is the register with those bits masked
 * off, plus what the caller asked for.
 */
static void portsc_write(struct xhci_ctrl *hc, int port, uint32_t set)
{
    uint32_t value = r32(hc, portsc_offset(hc, port));

    value &= ~(PORTSC_CHANGES | PORTSC_PED);
    w32(hc, portsc_offset(hc, port), value | set);
}

int xhci_ports(struct xhci_ctrl *hc)
{
    return hc ? (int)hc->caps.max_ports : 0;
}

int xhci_port_status(struct xhci_ctrl *hc, int port, struct usb_port_status *out)
{
    uint32_t value;

    if (port < 0 || (uint32_t)port >= hc->caps.max_ports) {
        return -1;
    }
    value = r32(hc, portsc_offset(hc, port));
    out->connected = (value & PORTSC_CCS) ? 1 : 0;
    out->enabled = (value & PORTSC_PED) ? 1 : 0;
    out->changed = hc->port_changed[port];
    out->speed = (int)((value >> PORTSC_SPEED_SHIFT) & PORTSC_SPEED_MASK);
    return 0;
}

int xhci_port_reset_begin(struct xhci_ctrl *hc, int port)
{
    uint32_t value;

    if (port < 0 || (uint32_t)port >= hc->caps.max_ports) {
        return -1;
    }
    value = r32(hc, portsc_offset(hc, port));
    if (!(value & PORTSC_CCS)) {
        return -1;
    }
    portsc_write(hc, port, PORTSC_PR);
    hc->port_deadline[port] = hc->now_ms + PORT_RESET_MS;
    return 0;
}

int xhci_port_reset_done(struct xhci_ctrl *hc, int port)
{
    uint32_t value;

    if (port < 0 || (uint32_t)port >= hc->caps.max_ports) {
        return -1;
    }
    value = r32(hc, portsc_offset(hc, port));
    if (value & PORTSC_PRC) {
        /* Clear the change and nothing else. */
        w32(hc, portsc_offset(hc, port), (value & ~(PORTSC_CHANGES | PORTSC_PED)) | PORTSC_PRC);
        return (value & PORTSC_PED) ? 1 : -1;
    }
    if (past(hc, hc->port_deadline[port])) {
        return -1;
    }
    return 0;
}

/**
 * Issue a command after the previous command or abort has resolved.
 *
 * After an abort, require both the stop event and CRR clear before resetting
 * the ring pointer. The stop event can name the next command's address; it
 * must be consumed before that address is reused.
 *
 * `slot` has the slot in its low byte and, for an endpoint command, the
 * endpoint's DCI in bits 16 to 20, where the command block has it too. The
 * last block of the ring is the Link back to its start: giving it the current
 * cycle hands it to the controller, and its toggle bit makes both sides
 * change cycle at the same place.
 */
int xhci_command_issue(struct xhci_ctrl *hc, uint32_t type, uint64_t parameter, uint32_t slot)
{
    uint32_t *trb;

    if (hc->command_running) {
        return -1;
    }
    if (hc->command_aborting) {
        if (!hc->command_stop_seen || (op_r32(hc, XHCI_CRCR) & CRCR_CRR)) {
            return -1;
        }
        hc->command_aborting = 0;
        op_w32(hc, XHCI_CRCR, trb_addr(trb_at(hc->cmd_ring, hc->cmd_enqueue)) | hc->cmd_cycle);
        op_w32(hc, XHCI_CRCR + 4, 0);
    }

    trb = trb_at(hc->cmd_ring, hc->cmd_enqueue);
    hc->command_trb = (uint32_t)(uintptr_t)trb;
    trb[0] = (uint32_t)(parameter & 0xFFFFFFFFu);
    trb[1] = (uint32_t)(parameter >> 32);
    trb[2] = 0;
    /* The slot goes in the top byte, the endpoint in bits 16 to 20. */
    trb[3] = ((type & TRB_TYPE_MASK) << TRB_TYPE_SHIFT) | ((slot & 0xFFu) << TRB_SLOT_SHIFT) |
             (slot & 0x1F0000u) | hc->cmd_cycle;

    hc->cmd_enqueue++;
    if (hc->cmd_enqueue == XHCI_RING_TRBS - 1) {
        uint32_t *link = trb_at(hc->cmd_ring, hc->cmd_enqueue);

        link[0] = (uint32_t)(uintptr_t)hc->cmd_ring;
        link[1] = 0;
        link[2] = 0;
        link[3] = (TRB_TYPE_LINK << TRB_TYPE_SHIFT) | TRB_TOGGLE | hc->cmd_cycle;
        hc->cmd_enqueue = 0;
        hc->cmd_cycle ^= TRB_CYCLE;
    }

    hc->command_running = 1;
    hc->command_finished = 0;
    hc->command_type = type;
    hc->command_deadline = hc->now_ms + COMMAND_MS;
    xhci_barrier();             /* the block is written before the controller is told */
    w32(hc, hc->caps.dboff, 0); /* doorbell zero: the command ring */
    return 0;
}

/**
 * Record the controller's state once per boot, at the first failure the core
 * reports after bring-up: the status and command ring registers, the
 * interrupter's, and the driver's own view of the command in flight. A boot
 * that lost its devices says here whether the controller halted, stopped its
 * ring, or was simply not asked.
 */
void xhci_state_record(struct xhci_ctrl *hc)
{
    static int recorded;

    if (recorded || !hc->running) {
        return;
    }
    recorded = 1;
    klog(KLOG_WARN, "usb",
         "state: usbsts %x crcr %x iman %x erdp %x; command running %d finished %d aborting %d "
         "type %d; events at %d cycle %d",
         op_r32(hc, XHCI_USBSTS), op_r32(hc, XHCI_CRCR),
         r32(hc, hc->caps.rtsoff + XHCI_IR0 + IR_IMAN),
         r32(hc, hc->caps.rtsoff + XHCI_IR0 + IR_ERDP), hc->command_running, hc->command_finished,
         hc->command_aborting, (int)hc->command_type, (int)hc->event_index, (int)hc->event_cycle);
    klog(
        KLOG_WARN, "usb", "state: event ring at %x, the block there %x %x %x %x, the one before %x",
        (unsigned)(uintptr_t)hc->event_ring, trb_at(hc->event_ring, hc->event_index)[0],
        trb_at(hc->event_ring, hc->event_index)[1], trb_at(hc->event_ring, hc->event_index)[2],
        trb_at(hc->event_ring, hc->event_index)[3],
        trb_at(hc->event_ring, hc->event_index == 0 ? XHCI_RING_TRBS - 1 : hc->event_index - 1)[3]);
}

/** Report a matching completion, retaining the command during an abort. */
int xhci_command_done(struct xhci_ctrl *hc, struct usb_command_result *out)
{
    if (!hc->command_running && !hc->command_finished) {
        return -1;
    }
    if (hc->command_finished) {
        hc->command_finished = 0;
        if (out) {
            *out = hc->command_result;
        }
        return hc->command_result.status == USB_XFER_OK ? 1 : -1;
    }
    if (!hc->command_aborting && past(hc, hc->command_deadline)) {
        command_abort(hc);
    }
    return 0;
}

/**
 * Request an abort without releasing the outstanding command.
 *
 * A matching completion may still return a slot. Keep its identity until
 * that event arrives or the Command Ring Stopped event resolves the abort.
 */
static void command_abort(struct xhci_ctrl *hc)
{
    hc->command_stop_seen = 0;
    hc->command_aborting = 1;
    op_w32(hc, XHCI_CRCR, CRCR_CA);
    op_w32(hc, XHCI_CRCR + 4, 0);
    klog(KLOG_ERR, "usb", "command: no answer; the ring was asked back");
}

int xhci_command_free(struct xhci_ctrl *hc)
{
    if (!hc->running || hc->command_running) {
        return 0;
    }
    if (hc->command_aborting && (!hc->command_stop_seen || (op_r32(hc, XHCI_CRCR) & CRCR_CRR))) {
        return 0;
    }
    return 1;
}

/**
 * Take one event and act on it.
 *
 * A command's completion ends the command in flight, and a slot an Enable
 * Slot has just given out is recorded here, where its number arrives. A
 * Command Ring Stopped event follows earlier command completions and names
 * the dequeue pointer, which need not be the outstanding command's address.
 * It resolves an unanswered abort without replacing a completed result. Port
 * changes are remembered for the port poller.
 *
 * A transfer event names its block by physical address; the block names the
 * transfer, and the transfer's record says what the block was to move. An
 * event for a slot not in use, or for no transfer this driver has, is stale
 * and dropped, the second counted: reporting it would let a slot handed to
 * the next device be advanced by the last one's transfer. The driver's own
 * Stop Endpoint, answered for the block the endpoint was on, retires and
 * delivers nothing, and its length is not a residual; the stop command's
 * completion is what the quiesce waits for.
 *
 * An event for a transfer's last block retires it: no block of it can be
 * fetched again (4.10.1.1.2). One for a control transfer's data stage, or for
 * a chained block that completed whole, records what moved and waits for the
 * last block's event. A short packet mid-transfer is reported at once, so the
 * layer above can go on, while the transfer stays the controller's until the
 * event for its last block, which a conforming controller posts and QEMU does
 * not, or until a stop.
 *
 * A failure on the bus (a stall, a transaction, babble or split error) halts
 * the endpoint; one the controller found in the block itself puts it in the
 * Error state, from which a Set TR Dequeue alone moves it on (4.8.3). The
 * blocks after the failing one are not fetched until a reset with its
 * dequeue retires the transfer. The bytes moved are those through the block
 * before, and the failing block's own where the code has a residual. A
 * control transfer's status stage failing after its data stage answered
 * fails the transfer, though the bytes the data stage recorded stand. Each
 * transfer is reported once.
 */
static void event_dispatch(struct xhci_ctrl *hc, const uint32_t *trb)
{
    uint32_t type = (trb[3] >> TRB_TYPE_SHIFT) & TRB_TYPE_MASK;
    uint32_t code = (trb[2] >> TRB_COMPLETION_SHIFT) & 0xFFu;

    if (type == TRB_TYPE_COMMAND_COMPLETE) {
        if (code == TRB_COMPLETION_RING_STOPPED) {
            if (!hc->command_aborting) {
                return;
            }
            hc->command_stop_seen = 1;
        } else if (trb[0] != hc->command_trb) {
            return;
        }
        if (!hc->command_running) {
            return;
        }
        hc->command_running = 0;
        hc->command_finished = 1;
        hc->command_result.status =
            (code == TRB_COMPLETION_SUCCESS) ? USB_XFER_OK : USB_XFER_FAILED;
        hc->command_result.code = (int)code;
        hc->command_result.device = (int)((trb[3] >> TRB_SLOT_SHIFT) & 0xFFu);
        if (code != TRB_COMPLETION_SUCCESS) {
            klog(KLOG_DEBUG, "usb", "command: slot %d refused with code %d",
                 hc->command_result.device, (int)code);
        }
        if (hc->command_type == TRB_TYPE_ENABLE_SLOT && hc->command_result.status == USB_XFER_OK &&
            hc->command_result.device > 0 && hc->command_result.device <= (int)hc->nslots &&
            hc->pending_port >= 0) {
            struct xhci_slot *s = &hc->slots[hc->command_result.device];

            s->used = 1;
            s->port = hc->pending_port;
            s->speed = hc->pending_speed;
            s->where = hc->pending_where;
            s->hub_ports = 0;
            s->hub_multi_tt = 0;
            s->hub_think_time = 0;
            hc->pending_port = -1;
        }
        return;
    }
    if (type == TRB_TYPE_PORT_CHANGE) {
        /* The port is in the top byte of the first word, numbered from one. */
        uint32_t port = ((trb[0] >> 24) & 0xFFu);

        if (port >= 1 && port <= hc->caps.max_ports) {
            hc->port_changed[port - 1] = 1;
        }
        return;
    }
    if (type == TRB_TYPE_TRANSFER_EVENT) {
        int device = (int)((trb[3] >> TRB_SLOT_SHIFT) & 0xFFu);
        uint32_t dci = (trb[3] >> 16) & 0x1Fu;
        uint32_t residual = trb[2] & 0xFFFFFFu;
        struct xhci_slot *s;
        struct xhci_td *t;
        uint32_t at, k, through, n;
        int last;

        if (device <= 0 || device > (int)hc->nslots || !hc->slots[device].used) {
            return;
        }
        s = &hc->slots[device];
        if (dci == 0 || dci >= XHCI_MAX_DCI || !s->ring[dci]) {
            return;
        }
        at = (uint32_t)((trb[0] - (uint32_t)s->ring_phys[dci]) / (TRB_WORDS * 4u));
        if (at >= XHCI_EP_RING_TRBS) {
            s->dropped_events++;
            return;
        }
        t = td_at(s, dci, at, &k);
        if (!t) {
            s->dropped_events++;
            return;
        }
        last = k + 1 == t->count;
        through = 0;
        for (n = 0; n <= k; n++) {
            through += t->len[n];
        }
        if (code == TRB_COMPLETION_STOPPED || code == TRB_COMPLETION_STOPPED_LEN ||
            code == TRB_COMPLETION_STOPPED_SHORT) {
            s->ep_state[dci] = XHCI_EP_STOPPED;
            return;
        }
        if (code == TRB_COMPLETION_SUCCESS || code == TRB_COMPLETION_SHORT) {
            if (t->outcome < 0) {
                t->moved = residual <= through ? through - residual : 0;
                if (t->control && !last && t->len[k] == 0) {
                    t->moved = 0; /* a setup block moves none of the data */
                }
                t->outcome = code == TRB_COMPLETION_SHORT ? USB_XFER_SHORT : USB_XFER_OK;
                t->code = (int)code;
            }
            if (last) {
                t->retired = 1;
                if (t->control && t->outcome == USB_XFER_OK) {
                    t->moved = t->total; /* the status stage's own bytes are none */
                }
            } else if (t->control || code == TRB_COMPLETION_SUCCESS) {
                return;
            }
        } else {
            uint32_t before = through - t->len[k];

            s->ep_state[dci] = (code == TRB_COMPLETION_STALL || code == TRB_COMPLETION_TXN_ERROR ||
                                code == TRB_COMPLETION_BABBLE || code == TRB_COMPLETION_SPLIT_ERROR)
                                   ? XHCI_EP_HALTED
                                   : XHCI_EP_ERROR;
            if (t->outcome < 0) {
                t->moved = before;
                if ((code == TRB_COMPLETION_STALL || code == TRB_COMPLETION_TXN_ERROR) &&
                    residual <= t->len[k]) {
                    t->moved = before + (t->len[k] - residual);
                }
            }
            if (t->outcome < 0 || t->control) {
                t->outcome = code == TRB_COMPLETION_STALL ? USB_XFER_STALLED : USB_XFER_FAILED;
                t->code = (int)code;
            }
        }
        if (t->delivered) {
            return;
        }
        {
            struct usb_completion *c = &hc->completions[hc->completion_head % XHCI_COMPLETIONS];

            c->hc = hc;
            c->device = device;
            c->endpoint = (int)dci;
            c->status = t->outcome;
            c->code = t->code;
            c->length = t->total;
            c->residual = t->total > t->moved ? t->total - t->moved : 0;
            c->generation = t->generation;
            t->delivered = 1;
            hc->completion_head++;
        }
        return;
    }
}

/**
 * Watch the ports for a connect settling.
 *
 * A contact bounces, and the specification asks for a hundred milliseconds of
 * quiet between a connect and anything done about it. This records when each
 * port last became connected, and `xhci_port_settled` is what the layer above
 * asks before it resets one.
 */
static void port_scan(struct xhci_ctrl *hc)
{
    uint32_t i;
    uint32_t limit = hc->caps.max_ports;

    for (i = 0; i < limit; i++) {
        int now_connected = (r32(hc, portsc_offset(hc, (int)i)) & PORTSC_CCS) ? 1 : 0;

        if (now_connected != hc->port_connected[i]) {
            hc->port_connected[i] = (uint8_t)now_connected;
            hc->port_connect_at[i] = hc->now_ms;
            hc->port_changed[i] = 1;
        }
    }
}

int xhci_port_settled(struct xhci_ctrl *hc, int port)
{
    if (port < 0 || (uint32_t)port >= hc->caps.max_ports) {
        return 0;
    }
    if (!hc->port_connected[port]) {
        return 0;
    }
    return past(hc, hc->port_connect_at[port] + PORT_DEBOUNCE_MS) ? 1 : 0;
}

/**
 * Turn the interrupter on and have the controller send `vector` as a message.
 *
 * In this order: the device's MSI capability, which is where the address and
 * the vector are written and which ends with the device's own enable; the
 * moderation interval to zero, because an interval the firmware left behind
 * would delay every event by that long; the interrupter's own enable; and
 * the controller's last. Nothing can be raised until the last two, so the
 * capability is written while the controller is quiet: a controller whose
 * firmware left MSI enabled with an address of its own would otherwise be
 * free to send a message, on the strength of enables just set, to wherever
 * that stale address points. `msi_on` is set before the enables, since the
 * controller answers a doorbell inside the write that rings it. The pending
 * bit is written back as zero: writing it as one clears it, and an interrupt
 * already pending is not this function's to acknowledge.
 *
 * Returns 0 when the controller will send messages, -1 when it will not, in
 * which case nothing here has been enabled.
 */
int xhci_interrupts_enable(struct xhci_ctrl *hc, uint8_t vector)
{
    uint32_t iman;

    hc->msi_on = 0;
    if (!hc->running) {
        return -1;
    }
    if (pci_msi_enable(&hc->pci, vector) != 0) {
        return -1;
    }
    hc->msi_on = 1;
    w32(hc, hc->caps.rtsoff + XHCI_IR0 + IR_IMOD, 0);
    iman = r32(hc, hc->caps.rtsoff + XHCI_IR0 + IR_IMAN);
    w32(hc, hc->caps.rtsoff + XHCI_IR0 + IR_IMAN, (iman & ~IMAN_IP) | IMAN_IE);
    op_w32(hc, XHCI_USBCMD, op_r32(hc, XHCI_USBCMD) | USBCMD_INTE);
    return 0;
}

/**
 * The IRQ of the controller's PCI line, 1 to 15, or -1 when it names none.
 */
int xhci_line_irq(struct xhci_ctrl *hc)
{
    if (!hc->running || hc->pci.irq < 1 || hc->pci.irq > 15) {
        return -1;
    }
    return (int)hc->pci.irq;
}

/**
 * Have the controller raise its interrupts on its PCI line.
 *
 * The path a machine takes with `rach2.nomsi`, or one whose processor has no
 * usable APIC: the line the firmware routed for the controller, named in its
 * configuration space, through the 8259. The line is level-triggered and
 * stays asserted until the handler clears `USBSTS.EINT` and `IMAN.IP`, which
 * `xhci_interrupt_ack` does; on the message path it clears `IMAN.IP` alone.
 * The Interrupt Disable bit of the command register is cleared here, since
 * firmware that set MSI up leaves it set.
 *
 * Returns the line's IRQ, 1 to 15, or -1 when the controller is not running
 * or names no line, in which case nothing was enabled.
 */
int xhci_interrupts_enable_line(struct xhci_ctrl *hc)
{
    uint32_t iman;

    hc->line_on = 0;
    if (!hc->running || hc->pci.irq < 1 || hc->pci.irq > 15) {
        return -1;
    }
    pci_enable_interrupts(&hc->pci);
    hc->line_on = 1;
    w32(hc, hc->caps.rtsoff + XHCI_IR0 + IR_IMOD, 0);
    iman = r32(hc, hc->caps.rtsoff + XHCI_IR0 + IR_IMAN);
    w32(hc, hc->caps.rtsoff + XHCI_IR0 + IR_IMAN, (iman & ~IMAN_IP) | IMAN_IE);
    op_w32(hc, XHCI_USBCMD, op_r32(hc, XHCI_USBCMD) | USBCMD_INTE);
    return (int)hc->pci.irq;
}

/**
 * Stop the interrupts, leaving the controller running and the timer to pump
 * it.
 *
 * The interrupter's enable goes first and the capability after, so that no
 * message can be raised between the two. A controller on its line needs only
 * the enables cleared.
 */
void xhci_interrupts_disable(struct xhci_ctrl *hc)
{
    uint32_t iman;

    if (!hc->msi_on && !hc->line_on) {
        return;
    }
    iman = r32(hc, hc->caps.rtsoff + XHCI_IR0 + IR_IMAN);
    w32(hc, hc->caps.rtsoff + XHCI_IR0 + IR_IMAN, iman & ~(IMAN_IE | IMAN_IP));
    op_w32(hc, XHCI_USBCMD, op_r32(hc, XHCI_USBCMD) & ~USBCMD_INTE);
    if (hc->msi_on) {
        pci_msi_disable(&hc->pci);
    }
    hc->msi_on = 0;
    hc->line_on = 0;
}

int xhci_interrupts_on(struct xhci_ctrl *hc)
{
    return hc->msi_on || hc->line_on;
}

/**
 * Acknowledge the interrupt at the controller.
 *
 * `IMAN.IP` is written as one to clear it, and the enable bit beside it is
 * written back as it was read: a bare write of the pending bit would clear
 * the enable with it and the controller would never raise another.
 *
 * On the line, `USBSTS.EINT` keeps the line asserted, and 5.4.2 has it
 * cleared before any pending bit; it is written alone, since the other bits
 * of that register are cleared by a one too and a zero leaves them. A
 * controller on messages has its status register left alone.
 *
 * This is the handler's and not the pump's. The timer calls the same pump
 * every tick and has nothing to acknowledge.
 */
void xhci_interrupt_ack(struct xhci_ctrl *hc)
{
    uint32_t iman;

    if (!hc->running) {
        return;
    }
    if (hc->line_on) {
        op_w32(hc, XHCI_USBSTS, USBSTS_EINT);
    }
    iman = r32(hc, hc->caps.rtsoff + XHCI_IR0 + IR_IMAN);
    w32(hc, hc->caps.rtsoff + XHCI_IR0 + IR_IMAN, iman | IMAN_IP);
}

/**
 * Take what an interrupt was raised for: acknowledge it, then drain.
 *
 * The two in one call because their order is the whole of it. An event the
 * controller writes between the drain and the acknowledgement would have its
 * pending bit cleared by that acknowledgement without having been read, and
 * would then wait for the timer's pump with no message coming for it. The
 * other way round there is no such window: whatever arrives after the
 * acknowledgement is either in this drain or raises an interrupt of its own.
 *
 * Returns what the drain took.
 */
int xhci_interrupt_take(struct xhci_ctrl *hc)
{
    xhci_interrupt_ack(hc);
    return xhci_events(hc);
}

/**
 * Drain the event ring, then look at the ports; return the events taken.
 *
 * A block whose cycle bit matches is whole: the bit is read first and the
 * rest after it, with a barrier between. A transfer event that finds the
 * completion queue full stays on the ring, unread, for the next pass, since
 * dropping it would leave whoever queued that transfer waiting for an answer
 * that never comes. One drain takes at most one ring's worth.
 *
 * The event ring's dequeue pointer is then written back with the busy bit,
 * after a drain that took events and after any drain while the interrupter
 * is on: the controller sets that bit when it raises an interrupt and raises
 * no other until the bit is written back as one, so a handler that drained
 * nothing, because the timer's pump took the event first, would otherwise
 * leave the controller silent from then on.
 */
int xhci_events(struct xhci_ctrl *hc)
{
    uint32_t taken = 0;
    uint32_t *trb;

    if (!hc->running) {
        return 0;
    }

    for (;;) {
        trb = trb_at(hc->event_ring, hc->event_index);
        if ((trb[3] & TRB_CYCLE) != hc->event_cycle) {
            break;
        }
        xhci_barrier();
        if (((trb[3] >> TRB_TYPE_SHIFT) & TRB_TYPE_MASK) == TRB_TYPE_TRANSFER_EVENT &&
            hc->completion_head - hc->completion_tail >= XHCI_COMPLETIONS) {
            break;
        }
        event_dispatch(hc, trb);
        taken++;
        hc->event_index++;
        if (hc->event_index == XHCI_RING_TRBS) {
            hc->event_index = 0;
            hc->event_cycle ^= TRB_CYCLE;
        }
        if (taken == XHCI_RING_TRBS) {
            break; /* a whole ring in one drain is enough for one turn */
        }
    }

    port_scan(hc);

    if (taken != 0 || hc->msi_on || hc->line_on) {
        w32(hc, hc->caps.rtsoff + XHCI_IR0 + IR_ERDP,
            trb_addr(trb_at(hc->event_ring, hc->event_index)) | ERDP_HANDLER);
        w32(hc, hc->caps.rtsoff + XHCI_IR0 + IR_ERDP + 4, 0);
    }
    return (int)taken;
}

int xhci_start(struct xhci_ctrl *hc)
{
    uint32_t i;
    uint32_t slots_enabled;
    uint32_t polls;

    hc->running = 0;
    hc->msi_on = 0; /* a controller being started has no interrupter yet */
    for (i = 0; i < XHCI_RING_TRBS * TRB_WORDS; i++) {
        hc->cmd_ring[i] = 0;
        hc->event_ring[i] = 0;
    }
    for (i = 0; i < hc->nslots + 1; i++) {
        hc->dcbaa[i] = 0;
    }
    hc->cmd_enqueue = 0;
    hc->cmd_cycle = TRB_CYCLE;
    hc->event_index = 0;
    hc->event_cycle = TRB_CYCLE;
    hc->command_running = 0;
    hc->command_finished = 0;
    hc->command_aborting = 0;
    hc->command_stop_seen = 0;
    hc->pending_port = -1;
    hc->completion_head = 0;
    hc->completion_tail = 0;
    for (i = 0; i <= hc->nslots; i++) {
        uint32_t j;

        hc->slots[i].used = 0;
        for (j = 0; j < XHCI_MAX_DCI; j++) {
            hc->slots[i].open[j] = 0;
        }
    }
    for (i = 0; i < hc->caps.max_ports; i++) {
        hc->port_changed[i] = 0;
        hc->port_deadline[i] = 0;
        hc->port_connect_at[i] = 0;
        hc->port_connected[i] = 0;
    }

    /* Entry 0 of the context base array is the scratchpad array's address. */
    if (hc->caps.scratchpads != 0) {
        hc->dcbaa[0] = (uint64_t)hc->scratchpad_array_phys;
    }

    slots_enabled = hc->nslots;
    op_w32(hc, XHCI_CONFIG, slots_enabled);

    op_w32(hc, XHCI_DCBAAP, (uint32_t)hc->dcbaa_phys);
    op_w32(hc, XHCI_DCBAAP + 4, 0);

    op_w32(hc, XHCI_CRCR, (uint32_t)(uintptr_t)hc->cmd_ring | CRCR_RCS);
    op_w32(hc, XHCI_CRCR + 4, 0);

    hc->erst[0] = (uint32_t)(uintptr_t)hc->event_ring;
    hc->erst[1] = 0;
    hc->erst[2] = XHCI_RING_TRBS;
    hc->erst[3] = 0;
    w32(hc, hc->caps.rtsoff + XHCI_IR0 + IR_ERSTSZ, 1);
    w32(hc, hc->caps.rtsoff + XHCI_IR0 + IR_ERDP, (uint32_t)(uintptr_t)hc->event_ring);
    w32(hc, hc->caps.rtsoff + XHCI_IR0 + IR_ERDP + 4, 0);
    w32(hc, hc->caps.rtsoff + XHCI_IR0 + IR_ERSTBA, (uint32_t)(uintptr_t)hc->erst);
    w32(hc, hc->caps.rtsoff + XHCI_IR0 + IR_ERSTBA + 4, 0);

    xhci_barrier(); /* every table is written before the controller runs */
    op_w32(hc, XHCI_USBCMD, op_r32(hc, XHCI_USBCMD) | USBCMD_RS);
    for (polls = 0; polls < XHCI_HALT_POLLS; polls++) {
        if (!(op_r32(hc, XHCI_USBSTS) & USBSTS_HCH)) {
            break;
        }
        hc->io->udelay(XHCI_POLL_US);
    }
    if (op_r32(hc, XHCI_USBSTS) & USBSTS_HCH) {
        hc->err = "the controller would not start";
        return -1;
    }

    /* A port without power reports no device, whatever is plugged in. */
    for (i = 0; i < hc->caps.max_ports; i++) {
        if (!(r32(hc, portsc_offset(hc, (int)i)) & PORTSC_PP)) {
            portsc_write(hc, (int)i, PORTSC_PP);
        }
    }

    hc->running = 1;
    hc->err = "none";
    return 0;
}

/**
 * Give the firmware back what `xhci_own` took: its system-management
 * interrupts as they were, and the controller, this kernel's claim dropped.
 * A kernel that took a controller and then could not drive it must not also
 * keep it.
 */
void xhci_release(struct xhci_ctrl *hc)
{
    if (!hc->legacy_taken) {
        return;
    }
    w32(hc, hc->legacy_offset + LEGCTLSTS, hc->legacy_saved_ctl);
    w32(hc, hc->legacy_offset, r32(hc, hc->legacy_offset) & ~LEGSUP_OS_OWNED);
    hc->legacy_taken = 0;
    klog(KLOG_INFO, "usb", "handoff: the controller was given back to the firmware");
}

/**
 * Attempt to halt the controller, polling for at most 16 ms.
 * Retain operating-system ownership during shutdown to avoid firmware legacy
 * reinitialization. Report a timeout if the halted bit remains clear. A
 * controller that is not running is given back to the firmware instead.
 */
void xhci_stop(struct xhci_ctrl *hc)
{
    uint32_t polls;

    if (!hc->running) {
        xhci_release(hc);
        return;
    }
    hc->running = 0;
    op_w32(hc, XHCI_USBCMD, op_r32(hc, XHCI_USBCMD) & ~(USBCMD_RS | USBCMD_INTE));
    for (polls = 0; polls < XHCI_HALT_POLLS; polls++) {
        if (op_r32(hc, XHCI_USBSTS) & USBSTS_HCH) {
            break;
        }
        hc->io->udelay(XHCI_POLL_US);
    }
    if (!(op_r32(hc, XHCI_USBSTS) & USBSTS_HCH)) {
        klog(KLOG_WARN, "usb", "stop: the controller would not halt");
        return;
    }
    klog(KLOG_INFO, "usb", "stop: the controller is halted");
}

/** A context entry, whose size the controller decides. */
static uint32_t *context_entry(struct xhci_ctrl *hc, void *base, uint32_t index)
{
    return (uint32_t *)base + index * (hc->caps.context_size / sizeof(uint32_t));
}

/** Zero a context entry, whatever size the controller uses. */
static void context_clear(struct xhci_ctrl *hc, uint8_t *base, uint32_t index)
{
    uint32_t *entry = context_entry(hc, base, index);
    uint32_t i;

    for (i = 0; i < hc->caps.context_size / 4u; i++) {
        entry[i] = 0;
    }
}

/**
 * The largest packet endpoint zero takes, by the device's speed.
 *
 * Low and high speed have one answer each. A full-speed device may take 8, 16,
 * 32 or 64 bytes and does not say which until it has been read, so 8 is
 * assumed and corrected once its descriptor arrives, which is what the
 * Evaluate Context command is for.
 */
static uint32_t default_packet_size(int speed)
{
    switch (speed) {
    case USB_SPEED_LOW:
        return 8;
    case USB_SPEED_HIGH:
        return 64;
    case USB_SPEED_SUPER:
        return 512;
    default:
        return 8;
    }
}

/**
 * The interval field the controller schedules from, derived from the interval
 * the device asked for.
 *
 * The two are not the same number and not even the same unit. A controller
 * counts in 125 microsecond units of two to the power of this field. A
 * low or full-speed device counts whole milliseconds, so its number is scaled
 * by eight and turned into a power; a high-speed device already gives a power,
 * one greater than the controller's.
 */
static uint32_t interval_for(int speed, int b_interval)
{
    uint32_t value = 0;

    if (b_interval <= 0) {
        b_interval = 1;
    }
    if (speed == USB_SPEED_HIGH || speed == USB_SPEED_SUPER) {
        value = (uint32_t)b_interval - 1u;
    } else {
        uint32_t frames = (uint32_t)b_interval;
        uint32_t power = 0;

        while ((1u << (power + 1)) <= frames) {
            power++;
        }
        value = power + 3u;
    }
    if (value > 15u) {
        value = 15u;
    }
    return value;
}

/**
 * Ask the controller for a slot for the device at `where`, an Enable Slot.
 *
 * The route string has four bits for each of five tiers; a location deeper
 * than that, a route with bits above its depth, or a translator outside the
 * slots and ports there are is refused, since a device addressed by a wrong
 * port is one the controller would talk to at random. The slot the controller
 * gives back is the handle, and which one it will be is not known until the
 * command answers, so the location and speed are kept against the command
 * rather than a slot; `event_dispatch` records them in the slot.
 */
int xhci_device_open(struct xhci_ctrl *hc, const struct usb_location *where, int speed)
{
    if (where == 0 || where->root_port < 0 || (uint32_t)where->root_port >= hc->caps.max_ports) {
        return -1;
    }
    if (where->depth < 0 || where->depth > USB_TREE_TIERS) {
        return -1;
    }
    if ((where->route >> (4 * where->depth)) != 0) {
        return -1;
    }
    if (where->tt_device < 0 || where->tt_device > (int)hc->nslots || where->tt_port < 0 ||
        where->tt_port > 255) {
        return -1;
    }
    hc->pending_port = where->root_port;
    hc->pending_speed = speed;
    hc->pending_where = *where;
    return xhci_command_issue(hc, TRB_TYPE_ENABLE_SLOT, 0, 0);
}

/**
 * The three words of a slot context that say where a device is and what it
 * is, written the same way by every command whose input has a slot context.
 *
 * Word 0: the route string in bits 0 to 19, the speed in 20 to 23, the
 * multi-TT bit 25, the hub bit 26, and the index of the last endpoint
 * context in 27 to 31. Word 1: the root port in bits 16 to 23 and a hub's
 * port count in 24 to 31. Word 2: for a low or full-speed device below a
 * high-speed hub, the translating hub's slot in bits 0 to 7 and its port in
 * 8 to 15; for a high-speed hub, its think time in bits 16 and 17. The
 * multi-TT bit is set on a hub that translates per port, and on a device
 * below such a hub.
 */
static void slot_words(const struct xhci_slot *s, uint32_t *slot, uint32_t last_ctx)
{
    uint32_t multi = (s->hub_multi_tt || s->where.tt_multi) ? 1u : 0u;

    slot[0] = (last_ctx << 27) | ((uint32_t)s->speed << 20) | (multi << 25) |
              ((s->hub_ports != 0 ? 1u : 0u) << 26) | (s->where.route & 0xFFFFFu);
    slot[1] = ((uint32_t)(s->port + 1) << 16) | ((uint32_t)s->hub_ports << 24);
    slot[2] = ((uint32_t)s->where.tt_device & 0xFFu) | (((uint32_t)s->where.tt_port & 0xFFu) << 8) |
              (((uint32_t)s->hub_think_time & 3u) << 16);
}

int xhci_device_hub(struct xhci_ctrl *hc, int device, int ports, int multi_tt, int think_time)
{
    uint32_t *control;
    uint32_t *slot;
    struct xhci_slot *s;
    uint32_t i;

    if (device <= 0 || device > (int)hc->nslots || !hc->slots[device].used ||
        xhci_command_busy(hc)) {
        return -1;
    }
    if (ports < 1 || ports > 255 || think_time < 0 || think_time > 3) {
        return -1;
    }
    s = &hc->slots[device];
    s->hub_ports = ports;
    s->hub_multi_tt = multi_tt ? 1 : 0;
    s->hub_think_time = think_time;
    for (i = 0; i <= XHCI_MAX_DCI; i++) {
        context_clear(hc, hc->input_ctx, i);
    }
    control = context_entry(hc, hc->input_ctx, 0);
    control[1] = 0x1u; /* the slot context alone */
    slot = context_entry(hc, hc->input_ctx, 1);
    slot_words(s, slot, 1u);
    xhci_barrier();
    return xhci_command_issue(hc, TRB_TYPE_EVALUATE_CTX, (uint64_t)(uintptr_t)hc->input_ctx,
                              (uint32_t)device);
}

/**
 * Take the pages a slot needs: its device context and the ring of endpoint
 * zero, one page each, read by the controller at their physical addresses.
 *
 * @return 0, or -1 when the heap has nothing to give.
 */
static int slot_pages(struct xhci_slot *s)
{
    if (!s->ctx) {
        s->ctx = (uint8_t *)kalloc_pages(1, &s->ctx_phys);
        if (!s->ctx) {
            return -1;
        }
        k_memset(s->ctx, 0, 4096);
    }
    if (!s->ring[1]) {
        s->ring[1] = (uint32_t *)kalloc_pages(1, &s->ring_phys[1]);
        if (!s->ring[1]) {
            return -1;
        }
    }
    return 0;
}

/**
 * Give back every page of a slot, and its transfer records. The controller
 * has finished with the slot by the time this runs, so nothing of its is left
 * pointing at them.
 */
static void slot_pages_free(struct xhci_slot *s)
{
    uint32_t i;

    for (i = 0; i < XHCI_MAX_DCI; i++) {
        if (s->ring[i]) {
            kfree_pages(s->ring[i], 1);
            s->ring[i] = 0;
            s->ring_phys[i] = 0;
        }
    }
    if (s->ctx) {
        kfree_pages(s->ctx, 1);
        s->ctx = 0;
        s->ctx_phys = 0;
    }
    for (i = 0; i < XHCI_TD_DCI; i++) {
        if (s->td[i]) {
            kfree(s->td[i]);
            s->td[i] = 0;
        }
    }
}

/**
 * Fill the input context for a device that has just been given a slot, and
 * ask the controller to address it.
 *
 * The input context is one, shared by every command that takes one, so
 * nothing is written into it while a command is in flight: filling it and
 * then finding the command ring busy would rewrite what that command had
 * given the controller. The ring of endpoint zero and the device context are
 * cleared before the controller is given them, since a slot reused for a
 * second device would otherwise hand it the first device's blocks, which have
 * the cycle value the controller looks for and addresses that are no longer
 * anybody's.
 */
int xhci_device_address(struct xhci_ctrl *hc, int device)
{
    uint32_t *control;
    uint32_t *slot;
    uint32_t *ep0;
    struct xhci_slot *s;
    uint32_t i;

    if (device <= 0 || device > (int)hc->nslots || !hc->slots[device].used) {
        return -1;
    }
    if (xhci_command_busy(hc)) {
        return -1;
    }
    s = &hc->slots[device];
    if (slot_pages(s) != 0) {
        return -1;
    }

    for (i = 0; i <= XHCI_MAX_DCI; i++) {
        context_clear(hc, hc->input_ctx, i);
    }
    control = context_entry(hc, hc->input_ctx, 0);
    control[1] = 0x3u; /* the slot context and endpoint zero */

    slot = context_entry(hc, hc->input_ctx, 1);
    slot_words(s, slot, 1u);

    ep0 = context_entry(hc, hc->input_ctx, 2);
    ep0[1] = (4u << 3) | (3u << 1) | (default_packet_size(s->speed) << 16);
    ep0[2] = (uint32_t)s->ring_phys[1] | TRB_CYCLE;
    ep0[3] = 0;
    ep0[4] = 8;

    for (i = 0; i < XHCI_EP_RING_TRBS * TRB_WORDS; i++) {
        s->ring[1][i] = 0;
    }
    for (i = 0; i < XHCI_MAX_DCI; i++) {
        context_clear(hc, s->ctx, i);
    }

    s->enqueue[1] = 0;
    s->cycle[1] = TRB_CYCLE;
    s->open[1] = 1;

    hc->dcbaa[device] = (uint64_t)s->ctx_phys;
    xhci_barrier();
    return xhci_command_issue(hc, TRB_TYPE_ADDRESS_DEVICE, (uint64_t)(uintptr_t)hc->input_ctx,
                              (uint32_t)device);
}

int xhci_device_packet_size(struct xhci_ctrl *hc, int device, int max_packet)
{
    uint32_t *control;
    uint32_t *ep0;
    uint32_t i;

    if (device <= 0 || device > (int)hc->nslots || !hc->slots[device].used ||
        xhci_command_busy(hc)) {
        return -1;
    }
    for (i = 0; i <= XHCI_MAX_DCI; i++) {
        context_clear(hc, hc->input_ctx, i);
    }
    control = context_entry(hc, hc->input_ctx, 0);
    control[1] = 0x2u; /* endpoint zero alone */
    ep0 = context_entry(hc, hc->input_ctx, 2);
    ep0[1] = (4u << 3) | (3u << 1) | ((uint32_t)max_packet << 16);
    xhci_barrier();
    return xhci_command_issue(hc, TRB_TYPE_EVALUATE_CTX, (uint64_t)(uintptr_t)hc->input_ctx,
                              (uint32_t)device);
}

/**
 * Close a device's endpoints to this driver and ask for its slot back, a
 * Disable Slot.
 *
 * Nothing new is queued on the endpoints from here, but the slot's context
 * and rings stay the controller's until the command completes: the slot
 * stays used until then, and `xhci_device_closed` is what finishes it.
 */
int xhci_device_close(struct xhci_ctrl *hc, int device)
{
    uint32_t i;

    if (device <= 0 || device > (int)hc->nslots || !hc->slots[device].used) {
        return -1;
    }
    for (i = 0; i < XHCI_MAX_DCI; i++) {
        hc->slots[device].open[i] = 0;
    }
    return xhci_command_issue(hc, TRB_TYPE_DISABLE_SLOT, 0, (uint32_t)device);
}

void xhci_device_closed(struct xhci_ctrl *hc, int device)
{
    if (device <= 0 || device > (int)hc->nslots) {
        return;
    }
    hc->slots[device].used = 0;
    hc->dcbaa[device] = 0;
    slot_pages_free(&hc->slots[device]);
}

int xhci_command_busy(struct xhci_ctrl *hc)
{
    return hc->command_running || hc->command_aborting;
}

/** The device context index of an endpoint, or zero when it is out of reach. */
static uint32_t dci_of(const struct usb_endpoint *ep)
{
    uint32_t dci;

    if (ep->number < 0 || ep->number > 15) {
        return 0;
    }
    if (ep->number == 0) {
        return 1;
    }
    dci = (uint32_t)ep->number * 2u + (ep->direction == USB_DIR_IN ? 1u : 0u);
    return dci < XHCI_MAX_DCI ? dci : 0;
}

/**
 * Add an endpoint to the device's configuration.
 *
 * Context Entries remains the highest device context index among all
 * configured endpoints, including the endpoint being added.
 */
int xhci_endpoint_open(struct xhci_ctrl *hc, int device, const struct usb_endpoint *ep)
{
    uint32_t dci = dci_of(ep);
    uint32_t last_ctx = dci;
    uint32_t *control;
    uint32_t *slot;
    uint32_t *entry;
    uint32_t type;
    uint32_t i;
    struct xhci_slot *s;

    if (device <= 0 || device > (int)hc->nslots || !hc->slots[device].used || dci == 0 ||
        xhci_command_busy(hc)) {
        return -1;
    }
    s = &hc->slots[device];
    if (!s->ring[dci]) {
        s->ring[dci] = (uint32_t *)kalloc_pages(1, &s->ring_phys[dci]);
        if (!s->ring[dci]) {
            return -1;
        }
    }

    for (i = 0; i <= XHCI_MAX_DCI; i++) {
        context_clear(hc, hc->input_ctx, i);
    }
    control = context_entry(hc, hc->input_ctx, 0);
    control[1] = 0x1u | (1u << dci); /* the slot context and this endpoint */

    for (i = dci + 1u; i < XHCI_MAX_DCI; i++) {
        if (s->open[i]) {
            last_ctx = i;
        }
    }
    slot = context_entry(hc, hc->input_ctx, 1);
    slot_words(s, slot, last_ctx);

    type = (ep->type == USB_EP_INTERRUPT) ? (ep->direction == USB_DIR_IN ? 7u : 3u)
                                          : (ep->direction == USB_DIR_IN ? 6u : 2u);
    entry = context_entry(hc, hc->input_ctx, dci + 1u);
    entry[0] = interval_for(s->speed, ep->interval) << 16;
    /* wMaxPacketSize bits 0-10 are the size; bits 11-12 are not used here. */
    entry[1] = (type << 3) | (3u << 1) | (((uint32_t)ep->max_packet & 0x7FFu) << 16);
    entry[2] = (uint32_t)s->ring_phys[dci] | TRB_CYCLE;
    entry[3] = 0;
    /* Average TRB Length, and Max ESIT Payload, which sets the bandwidth. */
    entry[4] = (uint32_t)ep->max_packet | ((uint32_t)ep->max_packet << 16);

    for (i = 0; i < XHCI_EP_RING_TRBS * TRB_WORDS; i++) {
        s->ring[dci][i] = 0;
    }
    s->enqueue[dci] = 0;
    s->cycle[dci] = TRB_CYCLE;
    s->open[dci] = 1;
    s->max_packet[dci] = ep->max_packet ? (uint32_t)ep->max_packet : 512u;
    s->ep_state[dci] = XHCI_EP_RUNNING;
    s->quiescing[dci] = 0;
    if (dci < XHCI_TD_DCI && !s->td[dci]) {
        s->td[dci] = kzalloc(sizeof(struct xhci_td) * XHCI_TD_RECORDS);
        if (!s->td[dci]) {
            s->open[dci] = 0;
            return -1;
        }
    }
    if (dci < XHCI_TD_DCI) {
        for (i = 0; i < XHCI_TD_RECORDS; i++) {
            s->td[dci][i].used = 0;
        }
    }
    xhci_barrier();
    return xhci_command_issue(hc, TRB_TYPE_CONFIG_EP, (uint64_t)(uintptr_t)hc->input_ctx,
                              (uint32_t)device);
}

void xhci_endpoint_close(struct xhci_ctrl *hc, int device, int endpoint)
{
    if (device > 0 && device < (int)hc->nslots && endpoint > 0 && endpoint < XHCI_MAX_DCI) {
        hc->slots[device].open[endpoint] = 0;
    }
}

/**
 * Reset a halted endpoint, a Reset Endpoint command. A halted endpoint keeps
 * its ring until it is reset, and the ring is then pointed at where this
 * driver writes next by a second command, which the caller issues
 * (`xhci_endpoint_dequeue_here`) once this one has answered.
 */
int xhci_endpoint_reset(struct xhci_ctrl *hc, int device, int endpoint)
{
    if (device <= 0 || device > (int)hc->nslots || endpoint <= 0 || endpoint >= XHCI_MAX_DCI) {
        return -1;
    }
    if (!hc->slots[device].used) {
        return -1;
    }
    return xhci_command_issue(hc, TRB_TYPE_RESET_EP, 0,
                              (uint32_t)device | ((uint32_t)endpoint << 16));
}

/** Put one block on an endpoint's ring, wrapping through its link block;
 * `chain` puts the chain bit on a Link written here, so a transfer that
 * goes on past the ring's end stays one transfer; `hold` writes the block
 * with the wrong cycle bit, to be published by its caller: the controller
 * stops at it until the caller flips the bit, which is how a transfer of
 * several blocks appears whole or not at all. */
static uint32_t *ring_put(struct xhci_ctrl *hc, int device, uint32_t dci, uint32_t w0, uint32_t w1,
                          uint32_t w2, uint32_t w3, int chain, int hold)
{
    struct xhci_slot *s = &hc->slots[device];
    uint32_t *ring = s->ring[dci];
    uint32_t *trb = &ring[s->enqueue[dci] * TRB_WORDS];

    trb[0] = w0;
    trb[1] = w1;
    trb[2] = w2;
    trb[3] = w3 | (hold ? (s->cycle[dci] ^ TRB_CYCLE) : s->cycle[dci]);
    s->enqueue[dci]++;
    if (s->enqueue[dci] == XHCI_EP_RING_TRBS - 1) {
        uint32_t *link = &ring[s->enqueue[dci] * TRB_WORDS];

        /* The Link has the ring's physical address, not the kernel's. */
        link[0] = (uint32_t)s->ring_phys[dci];
        link[1] = 0;
        link[2] = 0;
        link[3] = (TRB_TYPE_LINK << TRB_TYPE_SHIFT) | TRB_TOGGLE | (chain ? TRB_CHAIN : 0u) |
                  s->cycle[dci];
        s->enqueue[dci] = 0;
        s->cycle[dci] ^= TRB_CYCLE;
    }
    return trb;
}

/** The records of endpoint `dci`, or NULL for an endpoint that has none. */
static struct xhci_td *td_records(struct xhci_slot *s, uint32_t dci)
{
    if (dci >= XHCI_TD_DCI || !s->td[dci]) {
        return 0;
    }
    return s->td[dci];
}

/** Whether `t` is a transfer the controller may still reach. */
static int td_outstanding(const struct xhci_td *t)
{
    return t->used && !t->retired;
}

/** A free record of `dci`: unused, or retired and not held by its caller. */
static struct xhci_td *td_take(struct xhci_slot *s, uint32_t dci)
{
    struct xhci_td *r = td_records(s, dci);
    int i;

    if (!r) {
        return 0;
    }
    for (i = 0; i < XHCI_TD_RECORDS; i++) {
        if (!r[i].used || (r[i].retired && !r[i].held)) {
            return &r[i];
        }
    }
    return 0;
}

/** The outstanding transfer of `dci` with a block at ring index `at`, and
 * that block's place in it through `*k`. */
static struct xhci_td *td_at(struct xhci_slot *s, uint32_t dci, uint32_t at, uint32_t *k)
{
    struct xhci_td *r = td_records(s, dci);
    int i;
    uint32_t n;

    if (!r) {
        return 0;
    }
    for (i = 0; i < XHCI_TD_RECORDS; i++) {
        if (!td_outstanding(&r[i])) {
            continue;
        }
        for (n = 0; n < r[i].count; n++) {
            if (r[i].idx[n] == at) {
                *k = n;
                return &r[i];
            }
        }
    }
    return 0;
}

/** Whether ring index `at` of `dci` belongs to an outstanding transfer. */
static int td_holds(struct xhci_slot *s, uint32_t dci, uint32_t at)
{
    uint32_t k;

    return td_at(s, dci, at, &k) != 0;
}

/**
 * The physical address of `va`, for a controller that reads memory by its
 * physical address: the identity window's addresses as they are, a heap
 * page's through the kernel directory; 0 for one that has no frame.
 */
static uintptr_t td_phys(uintptr_t va)
{
    if (va < HARD_MEMORY_LIMIT) {
        return va;
    }
    return paging_virt_to_phys(va);
}

/**
 * The TD size field of a block: the packets of the transfer left after it,
 * counted in the endpoint's packet size, at most 31, and 0 on the last
 * block (xHCI 1.2, 4.11.2.4).
 */
static uint32_t td_size(uint32_t total, uint32_t through, uint32_t max_packet, int last)
{
    uint32_t packets, sent, left;

    if (last) {
        return 0;
    }
    packets = (total + max_packet - 1) / max_packet;
    sent = through / max_packet;
    left = packets > sent ? packets - sent : 0;
    return left > 31 ? 31 : left;
}

/**
 * Give the controller a transfer.
 *
 * A control transfer is a setup block, a data block when there is data and
 * a status block, one record over the three, with the event asked for on
 * the data block, which is the one that says how much came, and on the
 * status block, which is the transfer's last. A bulk or interrupt transfer is
 * a chain of Normal blocks, one per page-bounded piece of the buffer, each
 * with the piece's physical address and the packets left after it, the chain
 * bit on all but the last, an event on a short packet on the intermediate IN
 * blocks and on the last block's end (4.10.1.1.2, 4.11.7).
 *
 * The record and every translation are taken before a block is written,
 * and a transfer that cannot be described whole is refused with the ring
 * untouched. The first block is written with its cycle bit wrong, the rest
 * and a Link the controller may cross after it, and the first block's
 * cycle bit last, so a running endpoint cannot fetch part of a transfer
 * (4.11.7.1); the doorbell follows. A transfer whose blocks and Link would
 * wrap over its own start is refused before anything is published, as one
 * of more pieces than a record holds is; the control endpoint is opened by
 * the address command and not by `xhci_endpoint_open`, so its records are
 * taken here.
 */
int xhci_submit(struct xhci_ctrl *hc, int device, int endpoint, const struct usb_transfer *transfer)
{
    struct xhci_slot *s;
    struct xhci_td *t;
    uint32_t dci = (uint32_t)endpoint;
    uintptr_t phys[XHCI_TD_TRBS];
    uint32_t len[XHCI_TD_TRBS];
    uint32_t pieces = 0;
    uint32_t blocks;
    uint32_t at;
    uint32_t i;
    uint32_t through = 0;
    uint32_t *first = 0;

    if (device <= 0 || device > (int)hc->nslots || dci == 0 || dci >= XHCI_MAX_DCI) {
        return -1;
    }
    s = &hc->slots[device];
    if (!s->used || !s->open[dci] || s->quiescing[dci]) {
        return -1;
    }
    if (s->ep_state[dci] == XHCI_EP_HALTED || s->ep_state[dci] == XHCI_EP_ERROR) {
        return -1; /* nothing runs until the endpoint is reset or moved on */
    }
    s->asked[dci] = transfer->length;
    s->generation[dci] = transfer->generation;

    /* The pieces, translated before anything is written. */
    if (transfer->setup != 0) {
        if (transfer->length != 0) {
            phys[0] = td_phys((uintptr_t)transfer->buffer);
            len[0] = transfer->length;
            if (phys[0] == 0) {
                return -1;
            }
            pieces = 1;
        }
        blocks = 2 + pieces;
    } else {
        uint32_t off = 0;

        while (off < transfer->length) {
            uintptr_t va = (uintptr_t)transfer->buffer + off;
            uint32_t chunk = PAGE_SIZE - (uint32_t)(va & (PAGE_SIZE - 1));

            if (chunk > transfer->length - off) {
                chunk = transfer->length - off;
            }
            if (pieces == XHCI_TD_TRBS) {
                return -1;
            }
            phys[pieces] = td_phys(va);
            if (phys[pieces] == 0) {
                return -1;
            }
            len[pieces] = chunk;
            pieces++;
            off += chunk;
        }
        if (pieces == 0) {
            return -1;
        }
        blocks = pieces;
    }

    if (dci < XHCI_TD_DCI && !s->td[dci]) {
        s->td[dci] = kzalloc(sizeof(struct xhci_td) * XHCI_TD_RECORDS);
        if (!s->td[dci]) {
            return -1;
        }
    }
    t = td_take(s, dci);
    if (!t) {
        return -1;
    }
    if (blocks + 1 > XHCI_EP_RING_TRBS - 1) {
        return -1;
    }
    at = s->enqueue[dci];
    for (i = 0; i < blocks + 1; i++) {
        if (td_holds(s, dci, at)) {
            return -1;
        }
        at++;
        if (at >= XHCI_EP_RING_TRBS - 1) {
            at = 0;
        }
    }

    t->used = 1;
    t->retired = 0;
    t->held = 0;
    t->delivered = 0;
    t->control = transfer->setup != 0;
    t->outcome = -1;
    t->code = 0;
    t->generation = transfer->generation;
    t->total = transfer->length;
    t->moved = 0;
    t->count = 0;
    s->ep_state[dci] = XHCI_EP_RUNNING;

    if (transfer->setup != 0) {
        uint32_t setup_lo;
        uint32_t setup_hi;
        uint32_t trt = TRB_TRT_NO_DATA;

        setup_lo = (uint32_t)transfer->setup[0] | ((uint32_t)transfer->setup[1] << 8) |
                   ((uint32_t)transfer->setup[2] << 16) | ((uint32_t)transfer->setup[3] << 24);
        setup_hi = (uint32_t)transfer->setup[4] | ((uint32_t)transfer->setup[5] << 8) |
                   ((uint32_t)transfer->setup[6] << 16) | ((uint32_t)transfer->setup[7] << 24);
        if (transfer->length != 0) {
            trt = (transfer->direction == USB_DIR_IN) ? TRB_TRT_IN : TRB_TRT_OUT;
        }
        t->idx[t->count] = s->enqueue[dci];
        t->len[t->count] = 0;
        t->count++;
        first = ring_put(hc, device, dci, setup_lo, setup_hi, 8,
                         (TRB_TYPE_SETUP << TRB_TYPE_SHIFT) | TRB_IDT | trt, 0, 1);
        if (transfer->length != 0) {
            t->idx[t->count] = s->enqueue[dci];
            t->len[t->count] = transfer->length;
            t->count++;
            ring_put(hc, device, dci, (uint32_t)phys[0], (uint32_t)((uint64_t)phys[0] >> 32),
                     transfer->length,
                     (TRB_TYPE_DATA << TRB_TYPE_SHIFT) | TRB_IOC |
                         (transfer->direction == USB_DIR_IN ? TRB_DIR_IN : 0u),
                     0, 0);
        }
        /* The status stage runs the other way from the data stage. */
        t->idx[t->count] = s->enqueue[dci];
        t->len[t->count] = 0;
        t->count++;
        ring_put(
            hc, device, dci, 0, 0, 0,
            (TRB_TYPE_STATUS << TRB_TYPE_SHIFT) | TRB_IOC |
                ((transfer->length != 0 && transfer->direction == USB_DIR_IN) ? 0u : TRB_DIR_IN),
            0, 0);
    } else {
        /* A transfer with no piece was refused above. */
        i = 0;
        do {
            int last = i + 1 == pieces;
            uint32_t flags = TRB_TYPE_NORMAL << TRB_TYPE_SHIFT;
            uint32_t words; /* the length and, above it, the packets left */
            uint32_t *trb;

            through += len[i];
            words = len[i] | (td_size(transfer->length, through, s->max_packet[dci], last)
                              << TRB_TD_SIZE_SHIFT);
            if (last) {
                flags |= TRB_IOC;
            } else {
                flags |= TRB_CHAIN;
                if (transfer->direction == USB_DIR_IN) {
                    flags |= TRB_ISP;
                }
            }
            t->idx[t->count] = s->enqueue[dci];
            t->len[t->count] = len[i];
            t->count++;
            trb = ring_put(hc, device, dci, (uint32_t)phys[i], (uint32_t)((uint64_t)phys[i] >> 32),
                           words, flags, !last, i == 0);
            if (i == 0) {
                first = trb;
            }
            i++;
        } while (i < pieces);
    }

    xhci_barrier();
    first[3] ^= TRB_CYCLE;
    xhci_barrier();
    w32(hc, hc->caps.dboff + (uint32_t)device * 4u, dci);
    return 0;
}

int xhci_poll(struct xhci_ctrl *hc, struct usb_completion *out)
{
    if (hc->completion_head == hc->completion_tail) {
        return 0;
    }
    *out = hc->completions[hc->completion_tail % XHCI_COMPLETIONS];
    hc->completion_tail++;
    return 1;
}

/** The record of the transfer submitted under `generation` on `device`, or
 * NULL when the driver has none. */
static struct xhci_td *td_by_generation(struct xhci_ctrl *hc, int device, uint32_t generation)
{
    struct xhci_slot *s;
    uint32_t dci;
    int i;

    if (device <= 0 || device > (int)hc->nslots || !hc->slots[device].used) {
        return 0;
    }
    s = &hc->slots[device];
    for (dci = 1; dci < XHCI_TD_DCI; dci++) {
        struct xhci_td *r = s->td[dci];

        if (!r) {
            continue;
        }
        for (i = 0; i < XHCI_TD_RECORDS; i++) {
            if (r[i].used && r[i].generation == generation) {
                return &r[i];
            }
        }
    }
    return 0;
}

int xhci_td_retired(struct xhci_ctrl *hc, int device, uint32_t generation)
{
    struct xhci_td *t = td_by_generation(hc, device, generation);

    if (!t) {
        return -1;
    }
    return t->retired ? 1 : 0;
}

int xhci_td_hold(struct xhci_ctrl *hc, int device, uint32_t generation)
{
    struct xhci_td *t = td_by_generation(hc, device, generation);

    if (!t) {
        return -1;
    }
    t->held = 1;
    return 0;
}

void xhci_td_release(struct xhci_ctrl *hc, int device, uint32_t generation)
{
    struct xhci_td *t = td_by_generation(hc, device, generation);

    if (t) {
        t->held = 0;
        if (t->retired) {
            t->used = 0; /* nothing answers for this generation again */
        }
    }
}

int xhci_endpoint_state(struct xhci_ctrl *hc, int device, int endpoint)
{
    if (device <= 0 || device > (int)hc->nslots || endpoint <= 0 || endpoint >= XHCI_MAX_DCI ||
        !hc->slots[device].used) {
        return -1;
    }
    return hc->slots[device].ep_state[endpoint];
}

void xhci_endpoint_quiescing(struct xhci_ctrl *hc, int device, int endpoint, int on)
{
    if (device <= 0 || device > (int)hc->nslots || endpoint <= 0 || endpoint >= XHCI_MAX_DCI) {
        return;
    }
    hc->slots[device].quiescing[endpoint] = on ? 1 : 0;
}

int xhci_endpoint_stop(struct xhci_ctrl *hc, int device, int endpoint)
{
    if (device <= 0 || device > (int)hc->nslots || endpoint <= 0 || endpoint >= XHCI_MAX_DCI ||
        !hc->slots[device].used) {
        return -1;
    }
    return xhci_command_issue(hc, TRB_TYPE_STOP_EP, 0,
                              (uint32_t)device | ((uint32_t)endpoint << 16));
}

/**
 * Point the endpoint's ring at the entry this driver writes next, a Set TR
 * Dequeue with the driver's cycle state.
 *
 * That entry's cycle bit is still the opposite of the driver's, so the
 * controller pointed there finds nothing to fetch until the driver writes it;
 * the driver's own enqueue and cycle stay as they are.
 */
int xhci_endpoint_dequeue_here(struct xhci_ctrl *hc, int device, int endpoint)
{
    struct xhci_slot *s;
    uint64_t where;

    if (device <= 0 || device > (int)hc->nslots || endpoint <= 0 || endpoint >= XHCI_MAX_DCI ||
        !hc->slots[device].used || !hc->slots[device].ring[endpoint]) {
        return -1;
    }
    s = &hc->slots[device];
    where = (uint64_t)((uint32_t)s->ring_phys[endpoint] + s->enqueue[endpoint] * TRB_WORDS * 4u) |
            (uint64_t)s->cycle[endpoint];
    return xhci_command_issue(hc, TRB_TYPE_SET_TR_DEQUEUE, where,
                              (uint32_t)device | ((uint32_t)endpoint << 16));
}

void xhci_endpoint_retire_all(struct xhci_ctrl *hc, int device, int endpoint)
{
    struct xhci_td *r;
    int i;

    if (device <= 0 || device > (int)hc->nslots || endpoint <= 0 || endpoint >= XHCI_MAX_DCI ||
        !hc->slots[device].used) {
        return;
    }
    r = td_records(&hc->slots[device], (uint32_t)endpoint);
    if (!r) {
        return;
    }
    for (i = 0; i < XHCI_TD_RECORDS; i++) {
        if (r[i].used) {
            r[i].retired = 1;
        }
    }
    hc->slots[device].ep_state[endpoint] = XHCI_EP_STOPPED;
}

uint32_t xhci_dropped_events(struct xhci_ctrl *hc, int device)
{
    if (device <= 0 || device > (int)hc->nslots) {
        return 0;
    }
    return hc->slots[device].dropped_events;
}
