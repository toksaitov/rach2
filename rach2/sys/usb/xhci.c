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
/*
 * How many root ports this driver keeps state for. A controller may advertise
 * as many as 255; the two machines here have eight and fewer, and the state
 * below is a fixed table like every other in this kernel. A controller with
 * more ports than this is used as far as this many and says so, which is
 * better than refusing it and better than writing past the table.
 */
#define XHCI_MAX_PORTS 16

/*
 * A compiler barrier. The controller reads what this driver writes and writes
 * what it reads, so the order of a fill and the doorbell that publishes it
 * matters. x86 gives the store ordering; this stops the compiler from
 * reordering or eliding what it cannot see another party doing.
 */
#define xhci_barrier() __asm__ __volatile__("" : : : "memory")

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

    power_up(&dev);
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
    if (ports_end > XHCI_MAP_SIZE || doorbells_end > XHCI_MAP_SIZE ||
        runtime_end > XHCI_MAP_SIZE || caps.dboff == 0 || caps.rtsoff == 0) {
        xhci_err = "the controller's registers do not fit the mapped aperture";
        return -1;
    }

    if (caps.max_ports > XHCI_MAX_PORTS) {
        klog(KLOG_WARN, "usb", "ports: the controller has %d and this kernel reaches %d",
             (int)caps.max_ports, (int)XHCI_MAX_PORTS);
        caps.max_ports = XHCI_MAX_PORTS;
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

/* ---- the rings ---- */

/*
 * A transfer request block is four words: two of parameter, one of status, and
 * one of control whose lowest bit is the cycle bit and whose bits 15 to 10 are
 * the type. The cycle bit is how the controller and this driver tell whose
 * turn a block is: each writes blocks with the current cycle value and toggles
 * that value when it wraps past the end of its ring, so a block the other side
 * has not reached yet always reads as the wrong cycle.
 */
#define TRB_WORDS 4
#define TRB_CYCLE 0x1u
#define TRB_TYPE_SHIFT 10
#define TRB_TYPE_MASK 0x3Fu
#define TRB_TOGGLE 0x2u /* on a Link block: toggle the cycle here */
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
#define IR_ERSTSZ    0x08
#define IR_ERSTBA    0x10
#define IR_ERDP      0x18
#define ERDP_HANDLER 0x8u /* write it back to say the events were taken */

/* Port status and control, one per port, sixteen bytes apart. */
#define PORTSC_CCS   0x00000001u /* a device is connected */
#define PORTSC_PED   0x00000002u /* the port is enabled; write-one-to-CLEAR */
#define PORTSC_PR    0x00000010u /* reset the port */
#define PORTSC_PP    0x00000200u /* the port has power */
#define PORTSC_SPEED_SHIFT 10
#define PORTSC_SPEED_MASK  0xFu
/* Every change bit, all of them write-one-to-clear. A write that does not mask
 * these off clears whichever of them happened to be set, and the connect this
 * driver had not noticed yet is gone. */
#define PORTSC_CHANGES 0x00FE0000u
#define PORTSC_CSC     0x00020000u
#define PORTSC_PRC     0x00200000u

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

static uint32_t cmd_enqueue;  /* the block this driver writes next */
static uint32_t cmd_cycle;    /* the cycle value it writes */
static uint32_t event_index;  /* the block it reads next */
static uint32_t event_cycle;  /* the cycle value it expects there */

/* The command in flight, if any, and what it ended as. */
static int command_running;
static int command_finished;
static int command_aborting;
static struct usb_command_result command_result;
static uint32_t command_deadline;
static uint32_t command_trb;

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
    command_deadline = now_ms + COMMAND_MS;
    xhci_barrier(); /* the block is written before the controller is told */
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
        command_result.device = (int)((trb[3] >> TRB_SLOT_SHIFT) & 0xFFu);
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

void xhci_events(void)
{
    uint32_t taken = 0;
    uint32_t *trb;

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

    if (taken != 0) {
        /* Tell the controller how far this driver has read, and that it may
         * raise the next event. */
        w32(caps.rtsoff + XHCI_IR0 + IR_ERDP,
            (uint32_t)(uintptr_t)trb_at(event_ring, event_index) | ERDP_HANDLER);
        w32(caps.rtsoff + XHCI_IR0 + IR_ERDP + 4, 0);
    }
}

int xhci_start(void)
{
    uint32_t i;
    uint32_t slots;
    uint32_t polls;

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
    for (i = 0; i < XHCI_MAX_PORTS; i++) {
        port_changed[i] = 0;
        port_deadline[i] = 0;
        port_connect_at[i] = 0;
        port_connected[i] = 0;
    }
    command_aborting = 0;

    /* The scratchpad array is the controller's own memory, and the first entry
     * of the context array is where it looks for it. */
    if (caps.scratchpads != 0) {
        dcbaa[0] = (uint64_t)(uintptr_t)scratchpad_array;
    }

    slots = caps.max_slots < XHCI_SLOTS ? caps.max_slots : XHCI_SLOTS;
    op_w32(XHCI_CONFIG, slots);

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

    xhci_err = "none";
    return 0;
}

void xhci_stop(void)
{
    uint32_t polls;

    if (xhci_base == 0 && xhci_io == &xhci_mmio_io) {
        return; /* nothing was ever brought up */
    }
    op_w32(XHCI_USBCMD, op_r32(XHCI_USBCMD) & ~(USBCMD_RS | USBCMD_INTE));
    for (polls = 0; polls < XHCI_HALT_POLLS; polls++) {
        if (op_r32(XHCI_USBSTS) & USBSTS_HCH) {
            break;
        }
        xhci_io->udelay(XHCI_POLL_US);
    }
}
