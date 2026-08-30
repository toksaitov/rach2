/*
 * xhci.c: the USB host controller.
 *
 * The register interface every machine newer than about 2010 presents, and the
 * only one either of this project's two machines has. What it buys, beside
 * being present, is that one controller handles every speed: a low-speed
 * keyboard needs no companion controller, no transaction translator and no
 * split transactions, which is most of what a driver for the older interfaces
 * is made of.
 *
 * This file is bring-up and transport. It knows registers, rings, contexts and
 * commands, and knows nothing about descriptors, classes or what a keyboard
 * is. `core.c` above it knows those and no registers.
 *
 * Bring-up is the only part that waits, and it runs once at boot in thread
 * context. Everything after it is queued and polled, because the completions
 * are drained from the timer handler where a wait would stop the clock.
 */

#include "xhci.h"

#include "../klog.h"
#include "../mm/paging.h"
#include "../pci.h"
#include "../udelay.h"

/*
 * Capability registers, at the start of the aperture. The first word holds the
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

#define USBSTS_HCH 0x00000001u
#define USBSTS_CNR 0x00000800u

/*
 * Extended capabilities, a linked list in the aperture whose head
 * `HCCPARAMS1` names. Each entry's low byte is its identifier and its second
 * byte is the offset to the next one, in dwords, with zero ending the list.
 */
#define XECP_ID_LEGACY 1

/* The legacy support capability: who owns the controller, and the interrupts
 * the firmware was using to keep that ownership useful. */
#define LEGSUP_BIOS_OWNED 0x00010000u
#define LEGSUP_OS_OWNED   0x01000000u
#define LEGCTLSTS         0x04
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

static volatile uint8_t *xhci_base;
static struct xhci_caps caps;
static const char *xhci_err = "not tried";

/*
 * The scratchpad pages a controller asks for, and the array of their
 * addresses. Static and page-aligned: the tables in this driver are fixed, so
 * the memory can be too, and the controller reads both by physical address in
 * a kernel that identity-maps its own image.
 */
static uint64_t scratchpad_array[XHCI_MAX_SCRATCHPADS] __attribute__((aligned(64)));
static uint8_t scratchpad_pages[XHCI_MAX_SCRATCHPADS][4096] __attribute__((aligned(4096)));

static uint32_t xhci_mmio_r32(uint32_t offset)
{
    return *(volatile uint32_t *)(xhci_base + offset);
}

static void xhci_mmio_w32(uint32_t offset, uint32_t value)
{
    *(volatile uint32_t *)(xhci_base + offset) = value;
}

static const struct xhci_io xhci_mmio_io = {xhci_mmio_r32, xhci_mmio_w32, k_udelay};

static const struct xhci_io *xhci_io = &xhci_mmio_io;

void xhci_set_io(const struct xhci_io *ops)
{
    xhci_io = ops ? ops : &xhci_mmio_io;
}

static uint32_t r32(uint32_t offset)
{
    return xhci_io->r32(offset);
}

static void w32(uint32_t offset, uint32_t value)
{
    xhci_io->w32(offset, value);
}

/** An operational register, which lives past the capability registers. */
static uint32_t op_r32(uint32_t offset)
{
    return r32(caps.caplength + offset);
}

static void op_w32(uint32_t offset, uint32_t value)
{
    w32(caps.caplength + offset, value);
}

const struct xhci_caps *xhci_capabilities(void)
{
    return &caps;
}

const char *xhci_error(void)
{
    return xhci_err;
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
        }
    }
}

int xhci_discover(void)
{
    pci_device_t dev;
    pci_device_t skip;
    pci_device_t *skip_dev = 0;
    int found = 0;
    uint32_t base;

    while (pci_find_device_skip(PCI_CLASS_SERIAL_BUS, PCI_SUBCLASS_USB, skip_dev, &dev)) {
        if (dev.prog_if == XHCI_PCI_PROGIF) {
            found = 1;
            break;
        }
        skip = dev;
        skip_dev = &skip;
    }
    if (!found) {
        xhci_err = "no xHCI controller on the bus";
        return -1;
    }

    if (!pci_bar_is_memory(dev.bar[0])) {
        xhci_err = "the controller's aperture is not a memory window";
        return -1;
    }
    /* A 64-bit aperture is legal and common, and its upper half must be zero
     * here: this kernel addresses 32 bits, and taking the low half of a base
     * above 4 GiB would point the driver at some other device's registers. */
    if (pci_bar_is_64bit(dev.bar[0]) && dev.bar[1] != 0) {
        xhci_err = "the controller's aperture is above 4 GiB";
        return -1;
    }
    base = pci_bar_address(dev.bar[0]);
    if (base == 0) {
        xhci_err = "the controller's aperture was never programmed";
        return -1;
    }

    power_up(&dev);
    pci_enable_memory_space(&dev);
    pci_enable_bus_master(&dev);

    if (map_mmio(base, XHCI_MAP_SIZE) == 0) {
        xhci_err = "the controller's aperture could not be mapped";
        return -1;
    }
    xhci_base = (volatile uint8_t *)(uintptr_t)base;

    klog(KLOG_DEBUG, "usb", "found: %x:%x at %d:%d.%d, aperture 0x%X",
         (int)dev.vendor_id, (int)dev.device_id, (int)dev.bus, (int)dev.device,
         (int)dev.function, base);
    xhci_err = "none";
    return 0;
}

int xhci_own(void)
{
    uint32_t offset = caps.xecp;
    uint32_t polls;

    while (offset != 0) {
        uint32_t entry = r32(offset);
        uint32_t next = (entry >> 8) & 0xFFu;

        if ((entry & 0xFFu) == XECP_ID_LEGACY) {
            uint32_t control;

            if (!(entry & LEGSUP_BIOS_OWNED)) {
                /* Nobody else holds it. Say so and take the interrupts off
                 * anyway, because a firmware that released ownership may still
                 * have left its own callbacks enabled. */
                w32(offset, entry | LEGSUP_OS_OWNED);
            } else {
                w32(offset, entry | LEGSUP_OS_OWNED);
                for (polls = 0; polls < XHCI_HANDOFF_POLLS; polls++) {
                    if (!(r32(offset) & LEGSUP_BIOS_OWNED)) {
                        break;
                    }
                    xhci_io->udelay(XHCI_POLL_US);
                }
                if (r32(offset) & LEGSUP_BIOS_OWNED) {
                    xhci_err = "the firmware never gave up the controller";
                    return -1;
                }
            }

            control = r32(offset + LEGCTLSTS);
            control &= ~LEGCTLSTS_SMI_ENABLES;
            control |= LEGCTLSTS_SMI_EVENTS;
            w32(offset + LEGCTLSTS, control);
            xhci_err = "none";
            return 0;
        }

        if (next == 0) {
            break;
        }
        offset += next * 4;
    }

    /* No legacy capability: the controller was never the firmware's to hold. */
    xhci_err = "none";
    return 0;
}

int xhci_halt_reset(void)
{
    uint32_t polls;
    uint32_t cmd;

    cmd = op_r32(XHCI_USBCMD);
    op_w32(XHCI_USBCMD, cmd & ~(USBCMD_RS | USBCMD_INTE));
    for (polls = 0; polls < XHCI_HALT_POLLS; polls++) {
        if (op_r32(XHCI_USBSTS) & USBSTS_HCH) {
            break;
        }
        xhci_io->udelay(XHCI_POLL_US);
    }
    if (!(op_r32(XHCI_USBSTS) & USBSTS_HCH)) {
        xhci_err = "the controller never halted";
        return -1;
    }

    op_w32(XHCI_USBCMD, op_r32(XHCI_USBCMD) | USBCMD_HCRST);
    xhci_io->udelay(XHCI_RESET_PAUSE_US);
    for (polls = 0; polls < XHCI_RESET_POLLS; polls++) {
        if (!(op_r32(XHCI_USBCMD) & USBCMD_HCRST) && !(op_r32(XHCI_USBSTS) & USBSTS_CNR)) {
            break;
        }
        xhci_io->udelay(XHCI_POLL_US);
    }
    if (op_r32(XHCI_USBCMD) & USBCMD_HCRST) {
        xhci_err = "the controller's reset never completed";
        return -1;
    }
    if (op_r32(XHCI_USBSTS) & USBSTS_CNR) {
        xhci_err = "the controller never became ready";
        return -1;
    }

    xhci_err = "none";
    return 0;
}

int xhci_read_caps(void)
{
    uint32_t word;
    uint32_t ports_end;

    word = r32(XHCI_CAPLENGTH);
    caps.caplength = word & 0xFFu;
    caps.hciversion = (word >> 16) & 0xFFFFu;

    word = r32(XHCI_HCSPARAMS1);
    caps.max_slots = word & 0xFFu;
    caps.max_intrs = (word >> 8) & 0x7FFu;
    caps.max_ports = (word >> 24) & 0xFFu;

    /* The scratchpad count is split across the word, five bits in each half,
     * with the low five bits at the top. */
    word = r32(XHCI_HCSPARAMS2);
    caps.erst_max = (word >> 4) & 0xFu;
    caps.scratchpads = (((word >> 21) & 0x1Fu) << 5) | ((word >> 27) & 0x1Fu);

    word = r32(XHCI_HCCPARAMS1);
    caps.ac64 = (int)(word & 0x1u);
    caps.context_size = (word & 0x4u) ? 64u : 32u;
    caps.xecp = ((word >> 16) & 0xFFFFu) * 4u;

    caps.dboff = r32(XHCI_DBOFF) & ~0x3u;
    caps.rtsoff = r32(XHCI_RTSOFF) & ~0x1Fu;

    if (caps.caplength < 0x20u || caps.max_ports == 0 || caps.max_slots == 0) {
        xhci_err = "the controller's capability registers describe no controller";
        return -1;
    }

    /* Everything this driver reaches has to lie inside the window it mapped.
     * The ports are the far end of the operational registers, sixteen bytes
     * each; the doorbells and the runtime registers are wherever the
     * controller put them. */
    ports_end = caps.caplength + XHCI_PORTSC + caps.max_ports * 0x10u;
    if (ports_end > XHCI_MAP_SIZE || caps.dboff >= XHCI_MAP_SIZE ||
        caps.rtsoff >= XHCI_MAP_SIZE || caps.dboff == 0 || caps.rtsoff == 0) {
        xhci_err = "the controller's registers do not fit the mapped aperture";
        return -1;
    }

    caps.pagesize = op_r32(XHCI_PAGESIZE) & 0xFFFFu;
    if (!(caps.pagesize & 0x1u)) {
        xhci_err = "the controller does not take 4 KiB pages";
        return -1;
    }

    xhci_err = "none";
    return 0;
}

int xhci_scratchpads(void)
{
    uint32_t i;
    uint32_t j;

    if (caps.scratchpads > XHCI_MAX_SCRATCHPADS) {
        klog(KLOG_ERR, "usb", "scratchpad: the controller wants %d pages and this kernel has %d",
             (int)caps.scratchpads, (int)XHCI_MAX_SCRATCHPADS);
        xhci_err = "the controller wants more scratchpad pages than this kernel has";
        return -1;
    }

    for (i = 0; i < caps.scratchpads; i++) {
        for (j = 0; j < 4096; j++) {
            scratchpad_pages[i][j] = 0;
        }
        scratchpad_array[i] = (uint64_t)(uintptr_t)&scratchpad_pages[i][0];
    }

    xhci_err = "none";
    return 0;
}
