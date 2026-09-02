/*
 * xhci.c: the USB host controller.
 *
 * The register interface every machine newer than about 2010 presents, and the
 * only one either of this project's two machines has. One controller also
 * handles every speed, so a low-speed keyboard needs no companion controller,
 * no transaction translator and no split transactions, which is most of what a
 * driver for the older interfaces is made of.
 *
 * This file is bring-up and transport: registers, rings, contexts and commands.
 * Descriptors, classes and what a keyboard is belong to `core.c` above it,
 * which touches no register.
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
 * How many root ports this driver keeps state for. A controller may advertise
 * as many as 255; the two machines here have eight and fewer, and the state
 * below is a fixed table like every other in this kernel. A controller with
 * more ports than this is used as far as this many and says so, which is
 * better than refusing it and better than writing past the table.
 */
#define XHCI_MAX_PORTS   16

/*
 * A compiler barrier. The controller reads what this driver writes and writes
 * what it reads, so the order of a fill and the doorbell that publishes it
 * matters. x86 gives the store ordering; this stops the compiler from
 * reordering or eliding what it cannot see another party doing.
 */
#define xhci_barrier() __asm__ __volatile__("" : : : "memory")

/* Where the legacy-support capability is, and what it held before this kernel
 * touched it. Taking a controller from the firmware and then failing would
 * otherwise leave the firmware's interrupts off and its claim gone, with no
 * driver: on a machine whose firmware is emulating a PS/2 keyboard out of a
 * USB one, that is the keyboard. */
static uint32_t legacy_offset;
static uint32_t legacy_saved_sup;
static uint32_t legacy_saved_ctl;
static int legacy_taken;

static volatile uint8_t *xhci_base;
static struct xhci_caps caps;
static const char *xhci_err = "not tried";

/* Where the controller is on the bus, kept from the walk that found it: the
 * MSI capability is programmed through configuration space, long after. */
static pci_device_t xhci_pci;

/* Whether the controller has been told to send its interrupts as messages. */
static int msi_on;

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
            /* A function coming out of D3 is allowed ten milliseconds before
             * anything is asked of it, and a controller read during them
             * answers with ones. */
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
 * this milestone was written for, the firmware gave this controller two of nine
 * and the other seven were the two sockets a person actually plugs into.
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

int xhci_discover(void)
{
    pci_device_t dev;
    pci_device_t skip;
    pci_device_t *skip_dev = 0;
    int found = 0;
    uint32_t base;
    uintptr_t mapped;

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

    xhci_pci = dev;
    power_up(&dev);
    take_ports(&dev);
    pci_enable_memory_space(&dev);
    pci_enable_bus_master(&dev);

    /* `map_mmio` identity-maps and hands back the address to use. Taking what
     * it returned rather than the physical address again costs nothing and
     * leaves this driver correct if that ever stops being an identity map. */
    mapped = map_mmio(base, XHCI_MAP_SIZE);
    if (mapped == 0) {
        xhci_err = "the controller's aperture could not be mapped";
        return -1;
    }
    xhci_base = (volatile uint8_t *)mapped;

    klog(KLOG_DEBUG, "usb", "found: %x:%x at %d:%d.%d, aperture 0x%X", (int)dev.vendor_id,
         (int)dev.device_id, (int)dev.bus, (int)dev.device, (int)dev.function, base);
    xhci_err = "none";
    return 0;
}

int xhci_own(void)
{
    uint32_t offset = caps.xecp;
    uint32_t polls;

    /* The list is the controller's, so where it leads is checked rather than
     * trusted. A next-pointer only ever moves forward, and every step that
     * continues moves at least four bytes, so the mapped window is what ends
     * the walk: a list that reaches the end of it is refused rather than
     * followed into whatever the aperture is next to. */
    while (offset != 0) {
        uint32_t entry;
        uint32_t next;

        if (offset + 8u > XHCI_MAP_SIZE) {
            xhci_err = "the controller's capability list leaves its aperture";
            return -1;
        }
        entry = r32(offset);
        next = (entry >> 8) & 0xFFu;

        if ((entry & 0xFFu) == XECP_ID_LEGACY) {
            uint32_t control;

            legacy_offset = offset;
            legacy_saved_sup = entry;
            legacy_saved_ctl = r32(offset + LEGCTLSTS);
            klog(KLOG_DEBUG, "usb", "handoff: legsup 0x%X legctl 0x%X before", legacy_saved_sup,
                 legacy_saved_ctl);

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
            /* Ownership is both bits: the firmware's claim gone and this
             * kernel's in place. A controller that dropped the request keeps
             * the second one clear, and a driver that looked only at the first
             * would drive a controller it does not own. */
            if (!(r32(offset) & LEGSUP_OS_OWNED)) {
                xhci_err = "the controller did not take this kernel's claim";
                return -1;
            }

            control = r32(offset + LEGCTLSTS);
            control &= ~LEGCTLSTS_SMI_ENABLES;
            control |= LEGCTLSTS_SMI_EVENTS;
            w32(offset + LEGCTLSTS, control);
            legacy_taken = 1;
            klog(KLOG_DEBUG, "usb", "handoff: legsup 0x%X legctl 0x%X after", r32(offset),
                 r32(offset + LEGCTLSTS));
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

    /* A controller that says it is not ready may have no register written but
     * its own status, and a controller coming out of D3 or out of a reset the
     * firmware started can say so for milliseconds. Waiting here is what keeps
     * the first write below from landing in a controller that is still
     * starting. */
    for (polls = 0; polls < XHCI_RESET_POLLS; polls++) {
        if (!(op_r32(XHCI_USBSTS) & USBSTS_CNR)) {
            break;
        }
        xhci_io->udelay(XHCI_POLL_US);
    }
    if (op_r32(XHCI_USBSTS) & USBSTS_CNR) {
        xhci_err = "the controller was not ready to be halted";
        return -1;
    }

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
    uint32_t doorbells_end;
    uint32_t runtime_end;

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
    /* Each doorbell is a word, one per slot and one for the commands; each
     * interrupter is 32 bytes, and the first is 32 bytes into the runtime
     * registers. What must fit is the end of each set, not its start. */
    doorbells_end = caps.dboff + (caps.max_slots + 1u) * 4u;
    runtime_end = caps.rtsoff + 0x20u + caps.max_intrs * 0x20u;
    if (ports_end > XHCI_MAP_SIZE || doorbells_end > XHCI_MAP_SIZE || runtime_end > XHCI_MAP_SIZE ||
        caps.dboff == 0 || caps.rtsoff == 0) {
        xhci_err = "the controller's registers do not fit the mapped aperture";
        return -1;
    }

    if (caps.max_ports > XHCI_MAX_PORTS) {
        klog(KLOG_WARN, "usb", "ports: the controller has %d and this kernel reaches %d",
             (int)caps.max_ports, (int)XHCI_MAX_PORTS);
        caps.max_ports = XHCI_MAX_PORTS;
    }

    caps.pagesize = op_r32(XHCI_PAGESIZE) & 0xFFFFu;
    /* The words themselves, not only what was read out of them: a session at a
     * machine this kernel has never met takes the log away and needs the raw
     * values to compare with the datasheet. */
    klog(KLOG_DEBUG, "usb", "caps: hcs1 0x%X hcs2 0x%X hcc1 0x%X pagesize 0x%X",
         r32(XHCI_HCSPARAMS1), r32(XHCI_HCSPARAMS2), r32(XHCI_HCCPARAMS1), caps.pagesize);
    klog(KLOG_DEBUG, "usb", "caps: scratchpads %d, contexts %d bytes, 64-bit %d",
         (int)caps.scratchpads, (int)caps.context_size, caps.ac64);
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
        klog(KLOG_ERR, "usb", "scratchpad: the controller requires %d pages and this kernel has %d",
             (int)caps.scratchpads, (int)XHCI_MAX_SCRATCHPADS);
        xhci_err = "the controller requires more scratchpad pages than this kernel has";
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

/* ---- the rings ---- */

/*
 * A transfer request block is four words: two of parameter, one of status, and
 * one of control whose lowest bit is the cycle bit and whose bits 15 to 10 are
 * the type. The cycle bit is how the controller and this driver tell whose
 * turn a block is: each writes blocks with the current cycle value and toggles
 * that value when it wraps past the end of its ring, so a block the other side
 * has not reached yet always reads as the wrong cycle.
 */
#define TRB_WORDS      4
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

/* Operational registers this phase adds. */
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

/*
 * The rings, the context base array and the event ring segment table. All
 * static, aligned as the specification requires, and read by the controller at
 * their physical addresses, which in this kernel are the addresses it uses.
 */
static uint32_t cmd_ring[XHCI_RING_TRBS * TRB_WORDS] __attribute__((aligned(64)));
static uint32_t event_ring[XHCI_RING_TRBS * TRB_WORDS] __attribute__((aligned(64)));
static uint64_t dcbaa[XHCI_SLOTS + 1] __attribute__((aligned(64)));
static uint32_t erst[4] __attribute__((aligned(64)));

static uint32_t cmd_enqueue; /* the block this driver writes next */
static uint32_t cmd_cycle;   /* the cycle value it writes */
static uint32_t event_index; /* the block it reads next */
static uint32_t event_cycle; /* the cycle value it expects there */

/* The command in flight, if any, and what it ended as. */
/* Whether the controller is running. Nothing may be read from its event ring
 * before it is, and the timer calls into this driver from the first tick of
 * the boot, long before there is a controller to ask. */
static int xhci_running;

static int command_running;
static int command_finished;
static int command_aborting;
static struct usb_command_result command_result;
static uint32_t command_deadline;
static uint32_t command_trb;
static uint32_t command_type;

/* This driver's own clock, in milliseconds, and the per-port state that is
 * counted against it. */
static uint32_t now_ms;
static uint32_t port_deadline[XHCI_MAX_PORTS];
static uint32_t port_connect_at[XHCI_MAX_PORTS];
static uint8_t port_connected[XHCI_MAX_PORTS];
static uint8_t port_changed[XHCI_MAX_PORTS];

/**
 * Whether a moment has passed, in a way that survives the clock wrapping.
 *
 * The clock is milliseconds in 32 bits, which wraps after seven weeks. A plain
 * comparison against a deadline computed before the wrap says the deadline has
 * not arrived for another seven weeks; the difference, read as signed, says
 * what it should.
 */
static int past(uint32_t deadline)
{
    return (int32_t)(now_ms - deadline) >= 0;
}

/* Completions waiting to be reported. A transfer ends in an interrupt-free
 * drain, so what it ended as is kept until the layer above asks. */
#define XHCI_COMPLETIONS 8
static struct usb_completion completions[XHCI_COMPLETIONS];
static uint32_t completion_head;
static uint32_t completion_tail;

/* ---- devices, endpoints and transfers ---- */

/*
 * A device's context is an array of entries indexed by the endpoint's device
 * context index: one for the slot, then one per endpoint direction. Endpoint
 * zero is index one, and an endpoint numbered N is index 2N for the direction
 * out and 2N plus one for in. This kernel reaches endpoint numbers one to
 * three, which is index seven at the most, and refuses anything above that
 * rather than growing a table for devices it has no use for.
 */
/* Endpoint numbers run to 15 in each direction, so a device context has 31
 * endpoints and its index runs to 31; a stick picks its own numbers and the
 * ones it picks are its business, not this driver's. */
#define XHCI_MAX_DCI      32
#define XHCI_EP_RING_TRBS 16

/* Command and transfer block types this phase adds. */
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
#define TRB_TYPE_SET_TR_DEQUEUE 16

#define TRB_IOC         0x20u /* raise an event when this block is done */
#define TRB_IDT         0x40u /* the parameter is the data, not a pointer to it */
#define TRB_DIR_IN      0x10000u
#define TRB_TRT_NO_DATA 0u
#define TRB_TRT_OUT     (2u << 16)
#define TRB_TRT_IN      (3u << 16)

/* Completion codes this driver tells apart from every other refusal. */
#define TRB_COMPLETION_SHORT 13
#define TRB_COMPLETION_STALL 6

/*
 * The contexts and the rings. The input context is one, used for one command
 * at a time, which is all this driver issues. Each device has its own output
 * context and a ring for each endpoint it opens.
 */
static uint8_t input_ctx[(XHCI_MAX_DCI + 1) * 64] __attribute__((aligned(64)));
static uint8_t device_ctx[XHCI_SLOTS + 1][XHCI_MAX_DCI * 64] __attribute__((aligned(64)));
static uint32_t ep_ring[XHCI_SLOTS + 1][XHCI_MAX_DCI][XHCI_EP_RING_TRBS * TRB_WORDS]
    __attribute__((aligned(256)));

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
};

static struct xhci_slot slots[XHCI_SLOTS + 1];

/* The port and speed a slot is being taken for. The controller decides which
 * slot that will be and does not say so until the command answers, so what the
 * slot will describe is held here in the meantime. */
static int pending_port;
static int pending_speed;
static struct usb_location pending_where;

void xhci_advance(uint32_t milliseconds)
{
    now_ms += milliseconds;
}

/** A word of a block in a ring. */
static uint32_t *trb_at(uint32_t *ring, uint32_t index)
{
    return &ring[index * TRB_WORDS];
}

/**
 * The address of a port's status and control register.
 *
 * Ports are numbered from one in every xHCI document, and from zero in every
 * loop here, so the conversion happens in one place.
 */
static uint32_t portsc_offset(int port)
{
    return caps.caplength + XHCI_PORTSC + (uint32_t)port * 0x10u;
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
static void portsc_write(int port, uint32_t set)
{
    uint32_t value = r32(portsc_offset(port));

    value &= ~(PORTSC_CHANGES | PORTSC_PED);
    w32(portsc_offset(port), value | set);
}

int xhci_ports(void)
{
    return (int)caps.max_ports;
}

int xhci_port_status(int port, struct usb_port_status *out)
{
    uint32_t value;

    if (port < 0 || (uint32_t)port >= caps.max_ports) {
        return -1;
    }
    value = r32(portsc_offset(port));
    out->connected = (value & PORTSC_CCS) ? 1 : 0;
    out->enabled = (value & PORTSC_PED) ? 1 : 0;
    out->changed = port_changed[port];
    out->speed = (int)((value >> PORTSC_SPEED_SHIFT) & PORTSC_SPEED_MASK);
    return 0;
}

int xhci_port_reset_begin(int port)
{
    uint32_t value;

    if (port < 0 || (uint32_t)port >= caps.max_ports) {
        return -1;
    }
    value = r32(portsc_offset(port));
    if (!(value & PORTSC_CCS)) {
        return -1;
    }
    portsc_write(port, PORTSC_PR);
    port_deadline[port] = now_ms + PORT_RESET_MS;
    return 0;
}

int xhci_port_reset_done(int port)
{
    uint32_t value;

    if (port < 0 || (uint32_t)port >= caps.max_ports) {
        return -1;
    }
    value = r32(portsc_offset(port));
    if (value & PORTSC_PRC) {
        /* Clear the change and nothing else. */
        w32(portsc_offset(port), (value & ~(PORTSC_CHANGES | PORTSC_PED)) | PORTSC_PRC);
        return (value & PORTSC_PED) ? 1 : -1;
    }
    if (past(port_deadline[port])) {
        return -1;
    }
    return 0;
}

int xhci_command_issue(uint32_t type, uint64_t parameter, uint32_t slot)
{
    uint32_t *trb;

    if (command_running) {
        return -1;
    }
    if (command_aborting) {
        /* An abort is not finished when it is asked for. The controller stops
         * the ring and clears the running bit, and only then is there a ring
         * to put anything on. Where it stopped is not this driver's to know,
         * so the ring is pointed at the block it would have used next. */
        if (op_r32(XHCI_CRCR) & CRCR_CRR) {
            return -1;
        }
        command_aborting = 0;
        op_w32(XHCI_CRCR, (uint32_t)(uintptr_t)trb_at(cmd_ring, cmd_enqueue) | cmd_cycle);
        op_w32(XHCI_CRCR + 4, 0);
    }

    trb = trb_at(cmd_ring, cmd_enqueue);
    command_trb = (uint32_t)(uintptr_t)trb;
    trb[0] = (uint32_t)(parameter & 0xFFFFFFFFu);
    trb[1] = (uint32_t)(parameter >> 32);
    trb[2] = 0;
    trb[3] = ((type & TRB_TYPE_MASK) << TRB_TYPE_SHIFT) | (slot << TRB_SLOT_SHIFT) | cmd_cycle;

    cmd_enqueue++;
    if (cmd_enqueue == XHCI_RING_TRBS - 1) {
        /* The last block of the ring is the Link back to its start. Giving it
         * the current cycle hands it to the controller, and the toggle bit is
         * what makes both sides change cycle at the same place. */
        uint32_t *link = trb_at(cmd_ring, cmd_enqueue);

        link[0] = (uint32_t)(uintptr_t)cmd_ring;
        link[1] = 0;
        link[2] = 0;
        link[3] = (TRB_TYPE_LINK << TRB_TYPE_SHIFT) | TRB_TOGGLE | cmd_cycle;
        cmd_enqueue = 0;
        cmd_cycle ^= TRB_CYCLE;
    }

    command_running = 1;
    command_finished = 0;
    command_type = type;
    command_deadline = now_ms + COMMAND_MS;
    xhci_barrier();     /* the block is written before the controller is told */
    w32(caps.dboff, 0); /* doorbell zero: the command ring */
    return 0;
}

int xhci_command_done(struct usb_command_result *out)
{
    if (!command_running && !command_finished) {
        return -1;
    }
    if (command_finished) {
        command_finished = 0;
        if (out) {
            *out = command_result;
        }
        return command_result.status == USB_XFER_OK ? 1 : -1;
    }
    if (past(command_deadline)) {
        /* Ask for the ring back. The controller stops it and says so by
         * clearing the running bit; until then nothing may be issued, and any
         * completion that arrives belongs to the command being abandoned. */
        op_w32(XHCI_CRCR, CRCR_CA);
        op_w32(XHCI_CRCR + 4, 0);
        command_running = 0;
        command_aborting = 1;
        klog(KLOG_ERR, "usb", "command: no answer; the ring was asked back");
        return -1;
    }
    return 0;
}

/**
 * Take one event and act on it.
 *
 * Command completions end the command in flight, port changes are remembered
 * for the port poller, and transfer events belong to the phase that adds
 * endpoints.
 */
static void event_dispatch(const uint32_t *trb)
{
    uint32_t type = (trb[3] >> TRB_TYPE_SHIFT) & TRB_TYPE_MASK;
    uint32_t code = (trb[2] >> TRB_COMPLETION_SHIFT) & 0xFFu;

    if (type == TRB_TYPE_COMMAND_COMPLETE) {
        /* The event names the block whose command it belongs to. A completion
         * for anything else is the answer to a command already given up on,
         * and completing the one in flight with it would be answering the
         * wrong question. */
        if (!command_running || trb[0] != command_trb) {
            return;
        }
        command_running = 0;
        command_finished = 1;
        command_result.status = (code == TRB_COMPLETION_SUCCESS) ? USB_XFER_OK : USB_XFER_FAILED;
        command_result.code = (int)code;
        command_result.device = (int)((trb[3] >> TRB_SLOT_SHIFT) & 0xFFu);
        if (code != TRB_COMPLETION_SUCCESS) {
            klog(KLOG_DEBUG, "usb", "command: slot %d refused with code %d", command_result.device,
                 (int)code);
        }
        /* A slot the controller has just given out is recorded here, because
         * this is where its number arrives. */
        if (command_type == TRB_TYPE_ENABLE_SLOT && command_result.status == USB_XFER_OK &&
            command_result.device > 0 && command_result.device <= XHCI_SLOTS && pending_port >= 0) {
            struct xhci_slot *s = &slots[command_result.device];

            s->used = 1;
            s->port = pending_port;
            s->speed = pending_speed;
            s->where = pending_where;
            s->hub_ports = 0;
            s->hub_multi_tt = 0;
            s->hub_think_time = 0;
            pending_port = -1;
        }
        return;
    }
    if (type == TRB_TYPE_PORT_CHANGE) {
        /* The port is in the top byte of the first word, numbered from one. */
        uint32_t port = ((trb[0] >> 24) & 0xFFu);

        if (port >= 1 && port <= caps.max_ports) {
            port_changed[port - 1] = 1;
        }
        return;
    }
    if (type == TRB_TYPE_TRANSFER_EVENT) {
        struct usb_completion *c = &completions[completion_head % XHCI_COMPLETIONS];

        if (completion_head - completion_tail >= XHCI_COMPLETIONS) {
            klog(KLOG_WARN, "usb", "transfer: a completion was dropped");
            return;
        }
        c->device = (int)((trb[3] >> TRB_SLOT_SHIFT) & 0xFFu);
        c->endpoint = (int)((trb[3] >> 16) & 0x1Fu);
        if (c->device <= 0 || c->device > XHCI_SLOTS || !slots[c->device].used) {
            /* A device that has gone, answering for a transfer nobody is
             * waiting for. Reporting it would let a slot handed to the next
             * device be advanced by the last one's transfer. */
            return;
        }
        /* What the controller did not move is what it reports; what it did is
         * the difference. */
        c->length = (c->endpoint > 0 && c->endpoint < XHCI_MAX_DCI)
                        ? slots[c->device].asked[c->endpoint]
                        : 0;
        c->generation = (c->endpoint > 0 && c->endpoint < XHCI_MAX_DCI)
                            ? slots[c->device].generation[c->endpoint]
                            : 0;
        if (code == TRB_COMPLETION_SUCCESS) {
            c->status = USB_XFER_OK;
        } else if (code == TRB_COMPLETION_SHORT) {
            c->status = USB_XFER_SHORT;
        } else if (code == TRB_COMPLETION_STALL) {
            c->status = USB_XFER_STALLED;
        } else {
            c->status = USB_XFER_FAILED;
        }
        c->residual = trb[2] & 0xFFFFFFu;
        completion_head++;
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
static void port_scan(void)
{
    uint32_t i;
    uint32_t limit = caps.max_ports < XHCI_MAX_PORTS ? caps.max_ports : XHCI_MAX_PORTS;

    for (i = 0; i < limit; i++) {
        int now_connected = (r32(portsc_offset((int)i)) & PORTSC_CCS) ? 1 : 0;

        if (now_connected != port_connected[i]) {
            port_connected[i] = (uint8_t)now_connected;
            port_connect_at[i] = now_ms;
            port_changed[i] = 1;
        }
    }
}

int xhci_port_settled(int port)
{
    if (port < 0 || (uint32_t)port >= caps.max_ports) {
        return 0;
    }
    if (!port_connected[port]) {
        return 0;
    }
    return past(port_connect_at[port] + PORT_DEBOUNCE_MS) ? 1 : 0;
}

/**
 * Turn the interrupter on and have the controller send `vector` as a message.
 *
 * In this order: the device's MSI capability, which is where the address and
 * the vector are written and which ends with the device's own enable; the
 * moderation interval to zero, because a controller whose firmware left an
 * interval behind would hold an event back for as long as it says and this
 * driver wants the event when it happens; the interrupter's own enable; and
 * the controller's last. Nothing can be raised until the last two, so the
 * capability is written while the controller is quiet.
 *
 * Returns 0 when the controller will send messages, -1 when it will not, in
 * which case nothing here has been enabled and the timer stays the only pump.
 */
int xhci_interrupts_enable(uint8_t vector)
{
    uint32_t iman;

    msi_on = 0;
    if (!xhci_running) {
        return -1;
    }
    /*
     * The device's capability first, and the two enables after it. A
     * controller whose firmware left MSI enabled with an address of its own
     * would otherwise be free to send one, on the strength of enables this
     * function had just set, to wherever that stale address points. With the
     * interrupter's enable still clear nothing can be raised while the
     * capability is written.
     */
    if (pci_msi_enable(&xhci_pci, vector) != 0) {
        return -1;
    }
    /* The controller answers a doorbell inside the write that rings it, so
     * everything below is in place before the last enable. */
    msi_on = 1;
    w32(caps.rtsoff + XHCI_IR0 + IR_IMOD, 0);
    iman = r32(caps.rtsoff + XHCI_IR0 + IR_IMAN);
    /* The pending bit is written back as zero: it is cleared by writing it
     * as one, and an interrupt already pending is not this function's to
     * acknowledge. */
    w32(caps.rtsoff + XHCI_IR0 + IR_IMAN, (iman & ~IMAN_IP) | IMAN_IE);
    op_w32(XHCI_USBCMD, op_r32(XHCI_USBCMD) | USBCMD_INTE);
    return 0;
}

/**
 * Stop the messages, leaving the controller running and the timer to pump it.
 *
 * The interrupter's enable goes first and the capability after, so that no
 * message can be raised between the two.
 */
void xhci_interrupts_disable(void)
{
    uint32_t iman;

    if (!msi_on) {
        return;
    }
    msi_on = 0;
    iman = r32(caps.rtsoff + XHCI_IR0 + IR_IMAN);
    w32(caps.rtsoff + XHCI_IR0 + IR_IMAN, iman & ~(IMAN_IE | IMAN_IP));
    op_w32(XHCI_USBCMD, op_r32(XHCI_USBCMD) & ~USBCMD_INTE);
    pci_msi_disable(&xhci_pci);
}

int xhci_interrupts_on(void)
{
    return msi_on;
}

/**
 * Acknowledge the interrupt at the controller.
 *
 * `IMAN.IP` is written as one to clear it, and the enable bit beside it is
 * written back as it was read: a bare write of the pending bit would clear
 * the enable with it and the controller would never raise another.
 *
 * This is the handler's and not the pump's. The timer calls the same pump
 * every tick and has nothing to acknowledge.
 */
void xhci_interrupt_ack(void)
{
    uint32_t iman;

    if (!xhci_running) {
        return;
    }
    iman = r32(caps.rtsoff + XHCI_IR0 + IR_IMAN);
    w32(caps.rtsoff + XHCI_IR0 + IR_IMAN, iman | IMAN_IP);
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
int xhci_interrupt_take(void)
{
    xhci_interrupt_ack();
    return xhci_events();
}

int xhci_events(void)
{
    uint32_t taken = 0;
    uint32_t *trb;

    if (!xhci_running) {
        return 0;
    }

    for (;;) {
        trb = trb_at(event_ring, event_index);
        if ((trb[3] & TRB_CYCLE) != event_cycle) {
            break;
        }
        /* The cycle bit is the controller's promise that the rest of the block
         * is written. Read it first, then the rest, and let nothing move
         * across that point. */
        xhci_barrier();
        event_dispatch(trb);
        taken++;
        event_index++;
        if (event_index == XHCI_RING_TRBS) {
            event_index = 0;
            event_cycle ^= TRB_CYCLE;
        }
        if (taken == XHCI_RING_TRBS) {
            break; /* a whole ring in one drain is enough for one turn */
        }
    }

    port_scan();

    if (taken != 0 || msi_on) {
        /*
         * Tell the controller how far this driver has read, and that it may
         * raise the next event.
         *
         * The write happens on an empty drain too whenever the interrupter is
         * on, because the controller sets the event-handler-busy bit when it
         * raises an interrupt and will raise no other until that bit is
         * written back as one. A handler that drained nothing, because the
         * timer's pump took the event first, would otherwise leave the bit
         * set and the controller silent from then on.
         */
        w32(caps.rtsoff + XHCI_IR0 + IR_ERDP,
            (uint32_t)(uintptr_t)trb_at(event_ring, event_index) | ERDP_HANDLER);
        w32(caps.rtsoff + XHCI_IR0 + IR_ERDP + 4, 0);
    }
    return (int)taken;
}

int xhci_start(void)
{
    uint32_t i;
    uint32_t slots_enabled;
    uint32_t polls;

    xhci_running = 0;
    msi_on = 0; /* a controller being started has no interrupter yet */
    for (i = 0; i < XHCI_RING_TRBS * TRB_WORDS; i++) {
        cmd_ring[i] = 0;
        event_ring[i] = 0;
    }
    for (i = 0; i < XHCI_SLOTS + 1; i++) {
        dcbaa[i] = 0;
    }
    cmd_enqueue = 0;
    cmd_cycle = TRB_CYCLE;
    event_index = 0;
    event_cycle = TRB_CYCLE;
    command_running = 0;
    command_finished = 0;
    command_aborting = 0;
    pending_port = -1;
    completion_head = 0;
    completion_tail = 0;
    for (i = 0; i <= XHCI_SLOTS; i++) {
        uint32_t j;

        slots[i].used = 0;
        for (j = 0; j < XHCI_MAX_DCI; j++) {
            slots[i].open[j] = 0;
        }
    }
    for (i = 0; i < XHCI_MAX_PORTS; i++) {
        port_changed[i] = 0;
        port_deadline[i] = 0;
        port_connect_at[i] = 0;
        port_connected[i] = 0;
    }

    /* The scratchpad array is the controller's own memory, and the first entry
     * of the context array is where it looks for it. */
    if (caps.scratchpads != 0) {
        dcbaa[0] = (uint64_t)(uintptr_t)scratchpad_array;
    }

    slots_enabled = caps.max_slots < XHCI_SLOTS ? caps.max_slots : XHCI_SLOTS;
    op_w32(XHCI_CONFIG, slots_enabled);

    op_w32(XHCI_DCBAAP, (uint32_t)(uintptr_t)dcbaa);
    op_w32(XHCI_DCBAAP + 4, 0);

    op_w32(XHCI_CRCR, (uint32_t)(uintptr_t)cmd_ring | CRCR_RCS);
    op_w32(XHCI_CRCR + 4, 0);

    erst[0] = (uint32_t)(uintptr_t)event_ring;
    erst[1] = 0;
    erst[2] = XHCI_RING_TRBS;
    erst[3] = 0;
    w32(caps.rtsoff + XHCI_IR0 + IR_ERSTSZ, 1);
    w32(caps.rtsoff + XHCI_IR0 + IR_ERDP, (uint32_t)(uintptr_t)event_ring);
    w32(caps.rtsoff + XHCI_IR0 + IR_ERDP + 4, 0);
    w32(caps.rtsoff + XHCI_IR0 + IR_ERSTBA, (uint32_t)(uintptr_t)erst);
    w32(caps.rtsoff + XHCI_IR0 + IR_ERSTBA + 4, 0);

    xhci_barrier(); /* every table is written before the controller runs */
    op_w32(XHCI_USBCMD, op_r32(XHCI_USBCMD) | USBCMD_RS);
    for (polls = 0; polls < XHCI_HALT_POLLS; polls++) {
        if (!(op_r32(XHCI_USBSTS) & USBSTS_HCH)) {
            break;
        }
        xhci_io->udelay(XHCI_POLL_US);
    }
    if (op_r32(XHCI_USBSTS) & USBSTS_HCH) {
        xhci_err = "the controller would not start";
        return -1;
    }

    /* A port with no power reports no device whatever is plugged into it. A
     * controller that powers its own ports says so. */
    for (i = 0; i < caps.max_ports; i++) {
        if (!(r32(portsc_offset((int)i)) & PORTSC_PP)) {
            portsc_write((int)i, PORTSC_PP);
        }
    }

    xhci_running = 1;
    xhci_err = "none";
    return 0;
}

void xhci_release(void)
{
    if (!legacy_taken) {
        return;
    }
    /* Give the firmware back what it had: its own interrupts as they were, and
     * its claim on the controller. A kernel that took a controller and then
     * could not drive it must not also keep it. */
    w32(legacy_offset + LEGCTLSTS, legacy_saved_ctl);
    w32(legacy_offset, r32(legacy_offset) & ~LEGSUP_OS_OWNED);
    legacy_taken = 0;
    klog(KLOG_INFO, "usb", "handoff: the controller was given back to the firmware");
}

void xhci_stop(void)
{
    uint32_t polls;

    if (!xhci_running) {
        /* Nothing was started, so there is nothing to halt and no register
         * here may be written: a controller that refused its own reset, or
         * that was never found, is not this kernel's to touch. */
        xhci_release();
        return;
    }
    xhci_running = 0;
    op_w32(XHCI_USBCMD, op_r32(XHCI_USBCMD) & ~(USBCMD_RS | USBCMD_INTE));
    for (polls = 0; polls < XHCI_HALT_POLLS; polls++) {
        if (op_r32(XHCI_USBSTS) & USBSTS_HCH) {
            break;
        }
        xhci_io->udelay(XHCI_POLL_US);
    }
    if (!(op_r32(XHCI_USBSTS) & USBSTS_HCH)) {
        /* A controller still running as the machine restarts is what other
         * operating systems carry a quirk for on this exact part. Saying so is
         * all this kernel can do about it. */
        klog(KLOG_WARN, "usb", "stop: the controller would not halt");
    }

    /*
     * The controller is halted and it is not given back. Restoring the
     * firmware's interrupt sources and its claim wakes its system-management
     * handler, which re-initialises USB legacy support before this kernel's
     * next instruction runs: on the notebook this milestone was written for
     * that took twenty seconds, in the middle of a reboot, every time. The
     * firmware initialises USB again on the next boot whatever this kernel
     * leaves behind, so the wait gained nothing. A bring-up that fails is the
     * other case and still gives it back, because a machine with no USB driver
     * should keep whatever its firmware was doing for it.
     */
    klog(KLOG_INFO, "usb", "stop: the controller is halted");
}

/** A context entry, whose size the controller decides. */
static uint32_t *context_entry(uint8_t *base, uint32_t index)
{
    return (uint32_t *)(base + index * caps.context_size);
}

/** Zero a context entry, whatever size the controller uses. */
static void context_clear(uint8_t *base, uint32_t index)
{
    uint32_t *entry = context_entry(base, index);
    uint32_t i;

    for (i = 0; i < caps.context_size / 4u; i++) {
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

int xhci_device_open(const struct usb_location *where, int speed)
{
    if (where == 0 || where->root_port < 0 || (uint32_t)where->root_port >= caps.max_ports) {
        return -1;
    }
    /* The route string has four bits for each tier and five tiers; a route
     * with bits above its depth was built for a port that does not fit a
     * nibble, and a device addressed by a wrong port is one the controller
     * would talk to at random. */
    if (where->depth < 0 || where->depth > USB_TREE_TIERS) {
        return -1;
    }
    if ((where->route >> (4 * where->depth)) != 0) {
        return -1;
    }
    if (where->tt_device < 0 || where->tt_device > XHCI_SLOTS || where->tt_port < 0 ||
        where->tt_port > 255) {
        return -1;
    }
    /* The slot the controller gives back is the handle. Which one it will be
     * is not known until the command answers, so the location and speed are
     * held against the command rather than a slot. */
    pending_port = where->root_port;
    pending_speed = speed;
    pending_where = *where;
    return xhci_command_issue(TRB_TYPE_ENABLE_SLOT, 0, 0);
}

/**
 * The three words of a slot context that say where a device is and what it
 * is, written the same way by every command that carries the slot context.
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

int xhci_device_hub(int device, int ports, int multi_tt, int think_time)
{
    uint32_t *control;
    uint32_t *slot;
    struct xhci_slot *s;
    uint32_t i;

    if (device <= 0 || device > XHCI_SLOTS || !slots[device].used || xhci_command_busy()) {
        return -1;
    }
    if (ports < 1 || ports > 255 || think_time < 0 || think_time > 3) {
        return -1;
    }
    s = &slots[device];
    s->hub_ports = ports;
    s->hub_multi_tt = multi_tt ? 1 : 0;
    s->hub_think_time = think_time;
    for (i = 0; i <= XHCI_MAX_DCI; i++) {
        context_clear(input_ctx, i);
    }
    control = context_entry(input_ctx, 0);
    control[1] = 0x1u; /* the slot context alone */
    slot = context_entry(input_ctx, 1);
    slot_words(s, slot, 1u);
    xhci_barrier();
    return xhci_command_issue(TRB_TYPE_EVALUATE_CTX, (uint64_t)(uintptr_t)input_ctx,
                              (uint32_t)device);
}

/**
 * Fill the input context for a device that has just been given a slot, and
 * ask the controller to address it.
 */
int xhci_device_address(int device)
{
    uint32_t *control;
    uint32_t *slot;
    uint32_t *ep0;
    struct xhci_slot *s;
    uint32_t i;

    if (device <= 0 || device > XHCI_SLOTS || !slots[device].used) {
        return -1;
    }
    /* The input context is one, shared by every command that takes one, so
     * nothing is written into it until this command is certain of the ring.
     * Filling it first and being refused would rewrite what a command already
     * in flight had given the controller. */
    if (xhci_command_busy()) {
        return -1;
    }
    s = &slots[device];

    for (i = 0; i <= XHCI_MAX_DCI; i++) {
        context_clear(input_ctx, i);
    }
    control = context_entry(input_ctx, 0);
    control[1] = 0x3u; /* the slot context and endpoint zero */

    slot = context_entry(input_ctx, 1);
    slot_words(s, slot, 1u);

    ep0 = context_entry(input_ctx, 2);
    ep0[1] = (4u << 3) | (3u << 1) | (default_packet_size(s->speed) << 16);
    ep0[2] = (uint32_t)(uintptr_t)&ep_ring[device][1][0] | TRB_CYCLE;
    ep0[3] = 0;
    ep0[4] = 8;

    /* Both the ring and the context are cleared before the controller is
     * given them. A slot reused for a second device would otherwise hand it
     * the first device's blocks, which the controller would execute: they
     * carry the cycle value it is looking for and addresses that are no longer
     * anybody's. */
    for (i = 0; i < XHCI_EP_RING_TRBS * TRB_WORDS; i++) {
        ep_ring[device][1][i] = 0;
    }
    for (i = 0; i < XHCI_MAX_DCI; i++) {
        context_clear(device_ctx[device], i);
    }

    s->enqueue[1] = 0;
    s->cycle[1] = TRB_CYCLE;
    s->open[1] = 1;

    dcbaa[device] = (uint64_t)(uintptr_t)&device_ctx[device][0];
    xhci_barrier();
    return xhci_command_issue(TRB_TYPE_ADDRESS_DEVICE, (uint64_t)(uintptr_t)input_ctx,
                              (uint32_t)device);
}

int xhci_device_packet_size(int device, int max_packet)
{
    uint32_t *control;
    uint32_t *ep0;
    uint32_t i;

    if (device <= 0 || device > XHCI_SLOTS || !slots[device].used || xhci_command_busy()) {
        return -1;
    }
    for (i = 0; i <= XHCI_MAX_DCI; i++) {
        context_clear(input_ctx, i);
    }
    control = context_entry(input_ctx, 0);
    control[1] = 0x2u; /* endpoint zero alone */
    ep0 = context_entry(input_ctx, 2);
    ep0[1] = (4u << 3) | (3u << 1) | ((uint32_t)max_packet << 16);
    xhci_barrier();
    return xhci_command_issue(TRB_TYPE_EVALUATE_CTX, (uint64_t)(uintptr_t)input_ctx,
                              (uint32_t)device);
}

int xhci_device_close(int device)
{
    uint32_t i;

    if (device <= 0 || device > XHCI_SLOTS || !slots[device].used) {
        return -1;
    }
    /* The endpoints are closed to this driver at once, so nothing new is
     * queued on them, but what the controller holds is its own until the
     * command it is given says otherwise: the context stays where it is and
     * the slot stays used until Disable Slot completes. `xhci_device_closed`
     * is what finishes it. */
    for (i = 0; i < XHCI_MAX_DCI; i++) {
        slots[device].open[i] = 0;
    }
    return xhci_command_issue(TRB_TYPE_DISABLE_SLOT, 0, (uint32_t)device);
}

void xhci_device_closed(int device)
{
    if (device <= 0 || device > XHCI_SLOTS) {
        return;
    }
    slots[device].used = 0;
    dcbaa[device] = 0;
}

int xhci_command_busy(void)
{
    return command_running || command_aborting;
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

int xhci_endpoint_open(int device, const struct usb_endpoint *ep)
{
    uint32_t dci = dci_of(ep);
    uint32_t *control;
    uint32_t *slot;
    uint32_t *entry;
    uint32_t type;
    uint32_t i;
    struct xhci_slot *s;

    if (device <= 0 || device > XHCI_SLOTS || !slots[device].used || dci == 0 ||
        xhci_command_busy()) {
        return -1;
    }
    s = &slots[device];

    for (i = 0; i <= XHCI_MAX_DCI; i++) {
        context_clear(input_ctx, i);
    }
    control = context_entry(input_ctx, 0);
    control[1] = 0x1u | (1u << dci); /* the slot context and this endpoint */

    /* The slot context goes with every configure, and its highest used index
     * has to name this endpoint or the controller will not schedule it. */
    slot = context_entry(input_ctx, 1);
    slot_words(s, slot, dci);

    type = (ep->type == USB_EP_INTERRUPT) ? (ep->direction == USB_DIR_IN ? 7u : 3u)
                                          : (ep->direction == USB_DIR_IN ? 6u : 2u);
    entry = context_entry(input_ctx, dci + 1u);
    entry[0] = interval_for(s->speed, ep->interval) << 16;
    /* The endpoint descriptor's field is sixteen bits and the packet size is
     * the low eleven of them; the rest say how many such packets a high-speed
     * device may send in a microframe, which this kernel does not use. */
    entry[1] = (type << 3) | (3u << 1) | (((uint32_t)ep->max_packet & 0x7FFu) << 16);
    entry[2] = (uint32_t)(uintptr_t)&ep_ring[device][dci][0] | TRB_CYCLE;
    entry[3] = 0;
    /* The average block length and the most this endpoint may move in one
     * service interval, which is what the controller reserves bandwidth by. */
    entry[4] = (uint32_t)ep->max_packet | ((uint32_t)ep->max_packet << 16);

    for (i = 0; i < XHCI_EP_RING_TRBS * TRB_WORDS; i++) {
        ep_ring[device][dci][i] = 0;
    }
    s->enqueue[dci] = 0;
    s->cycle[dci] = TRB_CYCLE;
    s->open[dci] = 1;
    xhci_barrier();
    return xhci_command_issue(TRB_TYPE_CONFIG_EP, (uint64_t)(uintptr_t)input_ctx, (uint32_t)device);
}

void xhci_endpoint_close(int device, int endpoint)
{
    if (device > 0 && device < XHCI_SLOTS && endpoint > 0 && endpoint < XHCI_MAX_DCI) {
        slots[device].open[endpoint] = 0;
    }
}

int xhci_endpoint_reset(int device, int endpoint)
{
    if (device <= 0 || device > XHCI_SLOTS || endpoint <= 0 || endpoint >= XHCI_MAX_DCI) {
        return -1;
    }
    if (!slots[device].used) {
        return -1;
    }
    /* A halted endpoint keeps its ring until it is reset, and the ring is
     * then pointed at where this driver will write next. Both are commands;
     * this issues the first and the caller issues the second when it
     * finishes. */
    return xhci_command_issue(TRB_TYPE_RESET_EP, 0, (uint32_t)device | ((uint32_t)endpoint << 16));
}

int xhci_endpoint_dequeue(int device, int endpoint)
{
    struct xhci_slot *s;

    if (device <= 0 || device > XHCI_SLOTS || endpoint <= 0 || endpoint >= XHCI_MAX_DCI) {
        return -1;
    }
    if (!slots[device].used) {
        return -1;
    }
    s = &slots[device];
    s->enqueue[endpoint] = 0;
    s->cycle[endpoint] = TRB_CYCLE;
    return xhci_command_issue(TRB_TYPE_SET_TR_DEQUEUE,
                              (uint64_t)(uintptr_t)&ep_ring[device][endpoint][0] | TRB_CYCLE,
                              (uint32_t)device | ((uint32_t)endpoint << 16));
}

/** Put one block on an endpoint's ring, wrapping through its link block. */
static uint32_t *ring_put(int device, uint32_t dci, uint32_t w0, uint32_t w1, uint32_t w2,
                          uint32_t w3)
{
    struct xhci_slot *s = &slots[device];
    uint32_t *ring = &ep_ring[device][dci][0];
    uint32_t *trb = &ring[s->enqueue[dci] * TRB_WORDS];

    trb[0] = w0;
    trb[1] = w1;
    trb[2] = w2;
    trb[3] = w3 | s->cycle[dci];

    s->enqueue[dci]++;
    if (s->enqueue[dci] == XHCI_EP_RING_TRBS - 1) {
        uint32_t *link = &ring[s->enqueue[dci] * TRB_WORDS];

        link[0] = (uint32_t)(uintptr_t)ring;
        link[1] = 0;
        link[2] = 0;
        link[3] = (TRB_TYPE_LINK << TRB_TYPE_SHIFT) | TRB_TOGGLE | s->cycle[dci];
        s->enqueue[dci] = 0;
        s->cycle[dci] ^= TRB_CYCLE;
    }
    return trb;
}

int xhci_submit(int device, int endpoint, const struct usb_transfer *transfer)
{
    struct xhci_slot *s;
    uint32_t dci = (uint32_t)endpoint;

    if (device <= 0 || device > XHCI_SLOTS || dci == 0 || dci >= XHCI_MAX_DCI) {
        return -1;
    }
    s = &slots[device];
    if (!s->used || !s->open[dci]) {
        return -1;
    }

    s->asked[dci] = transfer->length;
    s->generation[dci] = transfer->generation;

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

        ring_put(device, dci, setup_lo, setup_hi, 8,
                 (TRB_TYPE_SETUP << TRB_TYPE_SHIFT) | TRB_IDT | trt);
        if (transfer->length == 0) {
            /* With no data stage there is nothing to report but the end, so
             * the status stage carries the event. */
            ring_put(device, dci, 0, 0, 0,
                     (TRB_TYPE_STATUS << TRB_TYPE_SHIFT) | TRB_IOC | TRB_DIR_IN);
            xhci_barrier();
            w32(caps.dboff + (uint32_t)device * 4u, dci);
            return 0;
        }
        if (transfer->length != 0) {
            /* The event is asked for on the data stage rather than the status
             * stage, because it is the data stage's event that says how much
             * arrived. A transfer whose event came from the status stage
             * reports nothing about the bytes, and a descriptor read short
             * would be parsed as if it were whole. */
            ring_put(device, dci, (uint32_t)(uintptr_t)transfer->buffer, 0, transfer->length,
                     (TRB_TYPE_DATA << TRB_TYPE_SHIFT) | TRB_IOC |
                         (transfer->direction == USB_DIR_IN ? TRB_DIR_IN : 0u));
        }
        /* The status stage goes the other way from the data, and carries the
         * event, so one transfer is one completion however many blocks it
         * took. */
        ring_put(
            device, dci, 0, 0, 0,
            (TRB_TYPE_STATUS << TRB_TYPE_SHIFT) |
                ((transfer->length != 0 && transfer->direction == USB_DIR_IN) ? 0u : TRB_DIR_IN));
    } else {
        ring_put(device, dci, (uint32_t)(uintptr_t)transfer->buffer, 0, transfer->length,
                 (TRB_TYPE_NORMAL << TRB_TYPE_SHIFT) | TRB_IOC);
    }

    xhci_barrier();
    w32(caps.dboff + (uint32_t)device * 4u, dci);
    return 0;
}

int xhci_poll(struct usb_completion *out)
{
    if (completion_head == completion_tail) {
        return 0;
    }
    *out = completions[completion_tail % XHCI_COMPLETIONS];
    completion_tail++;
    return 1;
}
