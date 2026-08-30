/*
 * xhci_hosttest.c: the xHCI driver against a model of a controller.
 *
 * QEMU's `qemu-xhci` is a controller that works. It resets when told, it has
 * no firmware holding it, it asks for no scratchpad pages and it puts its
 * registers where a driver expects them. The notebook's controller is an Intel
 * one behind firmware that may still own it, and no boot of either machine can
 * produce a reset that never finishes, firmware that never answers, or a
 * request for more scratchpad pages than a kernel has.
 *
 * So the driver is compiled natively and given a model of a controller through
 * the interface it reaches registers by. Each row drives one step of bring-up
 * and checks what that step returned and what it left in the registers, not
 * what it printed.
 *
 * The kernel's `uintptr_t` is 32 bits and this host's pointers are 64, so an
 * address the driver stores for the controller is truncated here. The rows
 * check such an address for being non-zero and page-aligned, and truncation
 * changes neither.
 */

extern int printf(const char *fmt, ...);

#include "../../rach2/sys/usb/xhci.h"

/* ---- the model ---- */

#define REGS 0x2000

static uint32_t reg[REGS];

/* Where this model's controller keeps things. A real controller chooses these
   and a driver must read them; the model puts them somewhere a driver that
   assumed the usual values would still be wrong. */
#define CAPLENGTH 0x20
#define XECP      0x500
#define DBOFF     0x800
#define RTSOFF    0x900

#define OP(off) (CAPLENGTH + (off))

#define R_CAPLENGTH  0x00
#define R_HCSPARAMS1 0x04
#define R_HCSPARAMS2 0x08
#define R_HCCPARAMS1 0x10
#define R_DBOFF      0x14
#define R_RTSOFF     0x18

#define O_USBCMD   0x00
#define O_USBSTS   0x04
#define O_PAGESIZE 0x08

/* The bits this model answers on, named apart from the driver's own names for
   them: the driver is included at the end of this file and defines its own. */
#define M_USBCMD_RS    0x1u
#define M_USBCMD_HCRST 0x2u
#define M_USBSTS_HCH   0x1u
#define M_USBSTS_CNR   0x800u

#define LEGSUP_BIOS_OWNED 0x00010000u
#define LEGSUP_OS_OWNED   0x01000000u
#define LEGCTL            (XECP + 4)
#define LEGCTL_EVENTS     0xE0000000u

/* What this run's controller does, set before each row. */
static int halts;
/* How many waits the firmware takes before it gives the controller up, so a
   row can have a driver that asks once and reads once fail where a driver
   that waits passes. */
static int handoff_delay;
static int reset_completes;
static int cnr_clears;
static int handoff_answers;

/* Whether the driver read a register between asserting the reset and waiting
   the millisecond an Intel controller needs. A driver that reads too early
   gets rubbish out of hardware that has not finished starting, and no
   emulator will ever show it. */
static int awaiting_pause;
static int read_too_early;

/* The power state the model's function is in, and what the driver wrote. */
static unsigned pm_state;

static uint32_t model_r32(uint32_t off)
{
    if (awaiting_pause) {
        read_too_early = 1;
    }
    if (off >= REGS) {
        return 0;
    }
    return reg[off];
}

static void model_w32(uint32_t off, uint32_t v)
{
    if (off >= REGS) {
        return;
    }

    if (off == OP(O_USBCMD)) {
        if (v & M_USBCMD_HCRST) {
            /* The reset bit clears itself when the controller has finished,
               and the controller is not ready until it says so. A model that
               cleared both at once would pass a driver that never looked at
               the ready bit. */
            awaiting_pause = 1;
            if (reset_completes) {
                v &= ~M_USBCMD_HCRST;
            }
            if (cnr_clears) {
                reg[OP(O_USBSTS)] &= ~M_USBSTS_CNR;
            } else {
                reg[OP(O_USBSTS)] |= M_USBSTS_CNR;
            }
        }
        if (!(v & M_USBCMD_RS) && halts) {
            reg[OP(O_USBSTS)] |= M_USBSTS_HCH;
        }
        if (v & M_USBCMD_RS) {
            reg[OP(O_USBSTS)] &= ~M_USBSTS_HCH;
        }
        reg[off] = v;
        return;
    }

    if (off == XECP) {
        /* The firmware answers a request for ownership by giving up its own
           claim. One that never answers leaves its bit set. */
        if ((v & LEGSUP_OS_OWNED) && handoff_answers && handoff_delay == 0) {
            v &= ~LEGSUP_BIOS_OWNED;
        } else if (v & LEGSUP_OS_OWNED) {
            v |= (reg[off] & LEGSUP_BIOS_OWNED);
        }
        reg[off] = v;
        return;
    }

    if (off == LEGCTL) {
        /* The event bits are write-one-to-clear and the enables are ordinary.
           Modelling the difference is what lets a row say the driver both
           turned the firmware's interrupts off and cleared what they had
           already raised. */
        uint32_t events = reg[off] & ~(v & LEGCTL_EVENTS);
        reg[off] = (v & ~LEGCTL_EVENTS) | (events & LEGCTL_EVENTS);
        return;
    }

    reg[off] = v;
}

static void model_udelay(uint32_t us)
{
    if (awaiting_pause && us >= 1000u) {
        awaiting_pause = 0;
    }
    if (handoff_delay > 0) {
        handoff_delay--;
        if (handoff_delay == 0 && handoff_answers) {
            reg[XECP] &= ~LEGSUP_BIOS_OWNED;
        }
    }
}

static const struct xhci_io model_io = {model_r32, model_w32, model_udelay};

/* ---- what the driver reaches for outside itself ---- */

#include "../../rach2/sys/pci.h"

static uint32_t model_bar0;
static uint32_t model_bar1;
static uint8_t model_progif;
static int model_present;

bool pci_find_device_skip(uint8_t cls, uint8_t sub, pci_device_t *skip, pci_device_t *out)
{
    (void)cls;
    (void)sub;
    if (skip || !model_present) {
        return 0; /* one controller on this bus */
    }
    out->vendor_id = 0x8086;
    out->device_id = 0x9c31;
    out->bus = 0;
    out->device = 20;
    out->function = 0;
    out->prog_if = model_progif;
    out->bar[0] = model_bar0;
    out->bar[1] = model_bar1;
    return 1;
}

bool pci_bar_is_64bit(uint32_t bar)
{
    return (bar & 0x6u) == 0x4u;
}

bool pci_bar_is_memory(uint32_t bar)
{
    return (bar & 1u) == 0;
}

uint32_t pci_bar_address(uint32_t bar)
{
    return bar & ~0xFu;
}

uint8_t pci_find_capability(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t id)
{
    (void)bus;
    (void)dev;
    (void)fn;
    return (uint8_t)(id == 0x01 ? 0x70 : 0);
}

uint16_t pci_read_config16(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off)
{
    (void)bus;
    (void)dev;
    (void)fn;
    if (off == 0x74) {
        return (uint16_t)pm_state;
    }
    return 0;
}

void pci_write_config16(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off, uint16_t v)
{
    (void)bus;
    (void)dev;
    (void)fn;
    if (off == 0x74) {
        pm_state = v & 0x3u;
    }
}

uint8_t pci_read_config8(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off)
{
    (void)bus;
    (void)dev;
    (void)fn;
    (void)off;
    return 0;
}

uint32_t pci_read_config32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off)
{
    (void)bus;
    (void)dev;
    (void)fn;
    (void)off;
    return 0;
}

void pci_enable_bus_master(pci_device_t *dev)
{
    (void)dev;
}

void pci_enable_memory_space(pci_device_t *dev)
{
    (void)dev;
}

/* The window the driver would map. Its address is never dereferenced here:
   every register access goes through the model. */
static uint8_t window[64];

uintptr_t map_mmio(uintptr_t phys, size_t size)
{
    (void)phys;
    (void)size;
    return (uintptr_t)(unsigned long)window;
}

void k_udelay(uint32_t us)
{
    (void)us;
}

void klog(int level, const char *subsystem, const char *format, ...)
{
    (void)level;
    (void)subsystem;
    (void)format;
}

#include "../../rach2/sys/usb/xhci.c"

/* ---- the rows ---- */

static int fails;

#define CHECK(what, cond)                 \
    do {                                  \
        if (!(cond)) {                    \
            printf("  FAIL: %s\n", what); \
            fails++;                      \
        }                                 \
    } while (0)

/* A controller that does everything right, before each row changes one thing
   about it. Sixteen slots, four ports, 64-byte contexts, no scratchpads, and
   a legacy capability the firmware has already released. */
static void model_reset(void)
{
    unsigned i;

    for (i = 0; i < REGS; i++) {
        reg[i] = 0;
    }
    reg[R_CAPLENGTH] = CAPLENGTH | (0x0110u << 16);
    reg[R_HCSPARAMS1] = 16u | (1u << 8) | (4u << 24);
    reg[R_HCSPARAMS2] = 0u;
    reg[R_HCCPARAMS1] = 0x1u | 0x4u | ((XECP / 4u) << 16);
    reg[R_DBOFF] = DBOFF;
    reg[R_RTSOFF] = RTSOFF;
    reg[OP(O_PAGESIZE)] = 0x1u;
    reg[OP(O_USBSTS)] = M_USBSTS_HCH;
    reg[XECP] = 1u; /* the legacy capability, ending the list */
    reg[LEGCTL] = 0u;

    halts = 1;
    handoff_delay = 0;
    reset_completes = 1;
    cnr_clears = 1;
    handoff_answers = 1;
    awaiting_pause = 0;
    read_too_early = 0;
    pm_state = 0;
    model_present = 1;
    model_progif = 0x30;
    model_bar0 = 0xFEBF0004u; /* a 64-bit memory aperture */
    model_bar1 = 0;
    xhci_set_io(&model_io);
}

int main(void)
{
    const struct xhci_caps *c;
    unsigned i;

    /* Finding the controller. */
    model_reset();
    CHECK("a controller with a memory aperture is taken", xhci_discover() == 0);

    model_reset();
    model_present = 0;
    CHECK("a bus with no xHCI is refused", xhci_discover() != 0);

    model_reset();
    model_progif = 0x20;
    CHECK("an EHCI is not mistaken for an xHCI", xhci_discover() != 0);

    model_reset();
    model_bar0 = 0xFEBF0001u;
    CHECK("an aperture in I/O space is refused", xhci_discover() != 0);

    model_reset();
    model_bar1 = 0x1u;
    CHECK("an aperture above 4 GiB is refused", xhci_discover() != 0);

    model_reset();
    model_bar0 = 0x4u;
    CHECK("an aperture that was never programmed is refused", xhci_discover() != 0);

    model_reset();
    pm_state = 3;
    CHECK("a controller parked in D3 is taken", xhci_discover() == 0);
    CHECK("and put back in D0, where its registers answer", pm_state == 0);

    /* What the controller says about itself. */
    model_reset();
    CHECK("the capability registers are read", xhci_read_caps() == 0);
    c = xhci_capabilities();
    CHECK("the operational registers begin where CAPLENGTH says", c->caplength == CAPLENGTH);
    CHECK("the slot count is read", c->max_slots == 16);
    CHECK("the port count is read", c->max_ports == 4);
    CHECK("the interrupter count is read", c->max_intrs == 1);
    CHECK("64-byte contexts are noticed", c->context_size == 64);
    CHECK("64-bit addressing is noticed", c->ac64 == 1);
    CHECK("the capability list is found", c->xecp == XECP);
    CHECK("the doorbells are found", c->dboff == DBOFF);
    CHECK("the runtime registers are found", c->rtsoff == RTSOFF);

    model_reset();
    reg[R_HCCPARAMS1] = 0x1u | ((XECP / 4u) << 16); /* CSZ clear */
    CHECK("32-byte contexts are read from the same field", xhci_read_caps() == 0);
    CHECK("and are 32 bytes", xhci_capabilities()->context_size == 32);

    /* The scratchpad count is five bits in each half of one word, with the
       low five at the top: a driver that read either half alone would ask for
       the wrong number of pages. */
    model_reset();
    reg[R_HCSPARAMS2] = (1u << 21) | (3u << 27);
    CHECK("a split scratchpad count is read", xhci_read_caps() == 0);
    CHECK("and is both halves", xhci_capabilities()->scratchpads == 35u);

    model_reset();
    reg[OP(O_PAGESIZE)] = 0x2u; /* 8 KiB only */
    CHECK("a controller that does not take 4 KiB pages is refused", xhci_read_caps() != 0);

    model_reset();
    reg[R_HCSPARAMS1] = 16u | (1u << 8) | (255u << 24);
    reg[R_DBOFF] = 0x20000u;
    CHECK("registers outside the mapped aperture are refused", xhci_read_caps() != 0);

    model_reset();
    reg[R_CAPLENGTH] = 0x10u;
    CHECK("a capability register set too short to be one is refused", xhci_read_caps() != 0);

    /* Taking the controller from the firmware. */
    model_reset();
    reg[XECP] = 1u | LEGSUP_BIOS_OWNED;
    reg[LEGCTL] = 0xE000FFFFu;
    CHECK("the capability list is read before it is walked", xhci_read_caps() == 0);
    CHECK("firmware that answers hands the controller over", xhci_own() == 0);
    CHECK("and its ownership bit is clear", (reg[XECP] & LEGSUP_BIOS_OWNED) == 0);
    CHECK("and this kernel's is set", (reg[XECP] & LEGSUP_OS_OWNED) != 0);
    CHECK("the firmware's interrupts are turned off", (reg[LEGCTL] & 0x0000E1FEu) == 0);
    CHECK("and what they had already raised is cleared", (reg[LEGCTL] & 0xE0000000u) == 0);

    model_reset();
    reg[XECP] = 1u | LEGSUP_BIOS_OWNED;
    handoff_delay = 200;
    CHECK("the capability list is read", xhci_read_caps() == 0);
    CHECK("firmware that answers slowly is waited for", xhci_own() == 0);
    CHECK("and hands the controller over in the end", (reg[XECP] & LEGSUP_BIOS_OWNED) == 0);

    model_reset();
    reg[XECP] = 1u | LEGSUP_BIOS_OWNED;
    handoff_answers = 0;
    CHECK("the capability list is read", xhci_read_caps() == 0);
    CHECK("firmware that never answers is refused", xhci_own() != 0);

    model_reset();
    reg[R_HCCPARAMS1] = 0x1u | 0x4u; /* no capability list at all */
    CHECK("a controller with no capability list is read", xhci_read_caps() == 0);
    CHECK("and is already this kernel's", xhci_own() == 0);

    model_reset();
    reg[XECP] = 2u | (2u << 8); /* a supported-protocol capability first */
    reg[XECP + 8] = 1u;         /* then the legacy one */
    reg[LEGCTL + 8] = 0u;
    CHECK("a list is walked to its end", xhci_read_caps() == 0);
    CHECK("and the legacy capability found past another", xhci_own() == 0);
    CHECK("with ownership taken there", (reg[XECP + 8] & LEGSUP_OS_OWNED) != 0);

    /* Halting and resetting. */
    model_reset();
    CHECK("the capability registers are read first", xhci_read_caps() == 0);
    CHECK("a controller that halts and resets is taken", xhci_halt_reset() == 0);
    CHECK("the reset pause is taken before it is read again", read_too_early == 0);
    CHECK("and it is left halted", (reg[OP(O_USBSTS)] & M_USBSTS_HCH) != 0);

    model_reset();
    halts = 0;
    CHECK("the capability registers are read", xhci_read_caps() == 0);
    reg[OP(O_USBSTS)] &= ~M_USBSTS_HCH;
    CHECK("a controller that never halts is refused", xhci_halt_reset() != 0);

    model_reset();
    reset_completes = 0;
    CHECK("the capability registers are read", xhci_read_caps() == 0);
    CHECK("a reset that never completes is refused", xhci_halt_reset() != 0);

    model_reset();
    cnr_clears = 0;
    CHECK("the capability registers are read", xhci_read_caps() == 0);
    CHECK("a controller that never becomes ready is refused", xhci_halt_reset() != 0);

    /* The scratchpad pages. */
    model_reset();
    CHECK("a controller that wants no scratchpads is served", xhci_read_caps() == 0);
    CHECK("with nothing to do", xhci_scratchpads() == 0);

    model_reset();
    reg[R_HCSPARAMS2] = (0u << 21) | (3u << 27);
    CHECK("a controller that wants three pages is read", xhci_read_caps() == 0);
    CHECK("and served", xhci_scratchpads() == 0);
    CHECK("with three pages", scratchpad_array[2] != 0 && scratchpad_array[3] == 0);
    for (i = 0; i < 3; i++) {
        CHECK("each page-aligned", (scratchpad_array[i] & 0xFFFu) == 0);
    }

    model_reset();
    reg[R_HCSPARAMS2] = (1u << 21) | (3u << 27); /* 35 pages */
    CHECK("a controller that wants more pages than this kernel has is read", xhci_read_caps() == 0);
    CHECK("and refused", xhci_scratchpads() != 0);

    if (fails == 0) {
        printf("xhci_hosttest: OK\n");
        return 0;
    }
    printf("xhci_hosttest: %d failures\n", fails);
    return 1;
}
