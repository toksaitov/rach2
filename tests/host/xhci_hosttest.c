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

/* Big enough to hold a register at the far end of the 64 KiB aperture the
   driver maps, so a row can put a capability list where its next step leaves
   the window. */
#define REGS 0x10010

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
/* Doorbell zero, which is the command ring's. */
#define M_DOORBELL0       DBOFF

/* A port's status and control register, and the bits of it this model answers
   on. The change bits and the enable bit are write-one-to-clear, which is what
   makes a careless read-modify-write throw away a connect and disable the
   port. */
#define M_PORTSC(port)   (OP(0x400u) + (port) * 0x10u)
#define M_PORTSC_CCS     0x00000001u
#define M_PORTSC_PED     0x00000002u
#define M_PORTSC_PR      0x00000010u
#define M_PORTSC_PP      0x00000200u
#define M_PORTSC_CHANGES 0x00FE0000u
#define M_PORTSC_CSC     0x00020000u
#define M_PORTSC_PRC     0x00200000u

/* The command ring control register's bits. Two are write-only and one is the
   controller's own. */
#define M_CRCR_CS     0x2u
#define M_CRCR_CA     0x4u
#define M_CRCR_CRR    0x8u
#define LEGCTL_EVENTS 0xE0000000u

/* What the controller does when a doorbell is rung, set after `xhci.c` is
   included because it reaches into the driver's own rings. */
static void (*on_doorbell)(uint32_t target);
/* Called when the driver points the command ring somewhere, which is how this
   model follows it after an abort. */
static void (*on_ring_moved)(void);
/* Called when a slot's endpoint doorbell is rung. */
static void (*on_endpoint_doorbell)(uint32_t slot, uint32_t dci);

/* What this run's controller does, set before each row. */
static int halts;
/* Whether a command the driver issues is answered at all. */
static int command_completes;
/* Whether the controller starts when it is told to. */
static int starts;
/* Whether a port reset the driver asks for ever finishes. */
static int port_reset_completes;
/* Whether the controller stops its command ring when it is asked to. */
static int abort_completes;
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

/* The power state the model's function is in. A function the firmware parked
   in D3 answers its configuration space and returns all ones from its
   registers, which is what makes reading them before waking it useless. */
static unsigned pm_state;

/* What the driver asked the bus for, so a row can require it. */
static int memory_space_enabled;
static int bus_master_enabled;
/* The class and subclass it searched for. */
static uint8_t searched_class;
static uint8_t searched_subclass;
/* Whether the firmware lets this kernel's claim stand at all. */
static int accepts_os_claim;
/* How many waits the controller takes to become ready, and whether it ever
   does. */
static int cnr_delay;
static int becomes_ready;

/* How many times the capability list's head was read: a walk that never ends
   reads it without limit, and one that gives up reads it a few times. */
static unsigned xecp_reads;

static uint32_t model_r32(uint32_t off)
{
    if (awaiting_pause) {
        read_too_early = 1;
    }
    if (off == XECP || off == XECP + 8) {
        xecp_reads++;
    }
    if (pm_state != 0) {
        return 0xFFFFFFFFu; /* a function in D3, answering nothing useful */
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
        if ((v & M_USBCMD_RS) && starts) {
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
        if (!accepts_os_claim) {
            /* A controller that drops the claim rather than taking it: the
               firmware's bit may clear and this kernel still does not own it. */
            v &= ~LEGSUP_OS_OWNED;
        }
        reg[off] = v;
        return;
    }

    if (off >= M_PORTSC(0) && off < M_PORTSC(16) && ((off - M_PORTSC(0)) % 0x10u) == 0) {
        uint32_t old = reg[off];
        uint32_t kept_changes = old & M_PORTSC_CHANGES & ~(v & M_PORTSC_CHANGES);
        uint32_t kept_enable = (v & M_PORTSC_PED) ? 0u : (old & M_PORTSC_PED);

        reg[off] = (v & ~(M_PORTSC_CHANGES | M_PORTSC_PED)) | kept_changes | kept_enable;
        if ((v & M_PORTSC_PR) && port_reset_completes) {
            /* A port whose reset finishes enables itself and says so. One
               whose reset never finishes leaves the reset bit standing. */
            reg[off] &= ~M_PORTSC_PR;
            reg[off] |= M_PORTSC_PED | M_PORTSC_PRC;
        }
        return;
    }

    if (off == M_DOORBELL0) {
        reg[off] = v;
        if (on_doorbell) {
            on_doorbell(v);
        }
        return;
    }

    if (off > M_DOORBELL0 && off < M_DOORBELL0 + (XHCI_SLOTS + 1) * 4u &&
        ((off - M_DOORBELL0) % 4u) == 0) {
        /* A doorbell for a slot's endpoint, which is how a transfer is
           published. Its number is the register's distance from the first. */
        reg[off] = v;
        if (on_endpoint_doorbell) {
            on_endpoint_doorbell((off - M_DOORBELL0) / 4u, v);
        }
        return;
    }

    if (off == OP(0x18u)) {
        /* The command ring register. The abort and stop bits are written by
           software and read as zero; the running bit belongs to the
           controller. A model that stored what was written would let a driver
           read back its own abort request and call it an answer. */
        uint32_t running = reg[off] & M_CRCR_CRR;

        if (v & M_CRCR_CA) {
            if (abort_completes) {
                running = 0;
            }
            reg[off] = running;
            return;
        }
        reg[off] = (v & ~(M_CRCR_CA | M_CRCR_CS | M_CRCR_CRR)) | running;
        if (on_ring_moved) {
            on_ring_moved();
        }
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
    if (cnr_delay > 0) {
        cnr_delay--;
        if (cnr_delay == 0 && becomes_ready) {
            reg[OP(O_USBSTS)] &= ~M_USBSTS_CNR;
        }
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
    searched_class = cls;
    searched_subclass = sub;
    if (cls != PCI_CLASS_SERIAL_BUS || sub != PCI_SUBCLASS_USB) {
        return 0; /* this bus has nothing else on it */
    }
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
    bus_master_enabled = 1;
}

void pci_enable_memory_space(pci_device_t *dev)
{
    (void)dev;
    memory_space_enabled = 1;
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

/* ---- what the controller does with what the driver gave it ---- */

/* Where the model writes its next event, and with which cycle value. It writes
   into the driver's own event ring, which is the memory the driver handed the
   controller, reached here by name rather than through the truncated address
   the driver wrote into `ERSTBA`. */
static uint32_t model_event_write;
static uint32_t model_event_cycle;

static void post_event(uint32_t type, uint32_t code, uint32_t slot, uint32_t param0)
{
    uint32_t *t = &event_ring[model_event_write * TRB_WORDS];

    t[0] = param0;
    t[1] = 0;
    t[2] = code << TRB_COMPLETION_SHIFT;
    t[3] = (type << TRB_TYPE_SHIFT) | (slot << TRB_SLOT_SHIFT) | model_event_cycle;
    model_event_write++;
    if (model_event_write == XHCI_RING_TRBS) {
        model_event_write = 0;
        model_event_cycle ^= TRB_CYCLE;
    }
}

/* The command the driver last wrote, so a row can require the right one was
   issued rather than merely that something was. */
static uint32_t last_command_type;
static uint32_t last_command_slot;
static uint32_t commands_seen;

/* Where the controller has read up to on the command ring, and the cycle value
   it expects there. Following the ring rather than guessing which block the
   driver just wrote is what makes the wrap rows mean anything: a model that
   assumed the block before the enqueue pointer would read the link block as a
   command every time the ring wrapped. */
static uint32_t model_cmd_dequeue;
static uint32_t model_cmd_cycle;
/* The next slot this controller will give out. A real one chooses, and says
   which in the completion; a model that answered zero would let a driver that
   ignored the answer pass. */
static uint32_t model_next_slot;

static void doorbell_rung(uint32_t target)
{
    if (target != 0) {
        return;
    }

    for (;;) {
        const uint32_t *t = &cmd_ring[model_cmd_dequeue * TRB_WORDS];
        uint32_t type;

        if ((t[3] & TRB_CYCLE) != model_cmd_cycle) {
            break;
        }
        type = (t[3] >> TRB_TYPE_SHIFT) & TRB_TYPE_MASK;
        if (type == TRB_TYPE_LINK) {
            model_cmd_dequeue = 0;
            if (t[3] & TRB_TOGGLE) {
                model_cmd_cycle ^= TRB_CYCLE;
            }
            continue;
        }

        last_command_type = type;
        last_command_slot = (t[3] >> TRB_SLOT_SHIFT) & 0xFFu;
        commands_seen++;
        reg[OP(0x18u)] |= M_CRCR_CRR;
        if (command_completes) {
            uint32_t answered = last_command_slot;

            if (last_command_type == TRB_TYPE_ENABLE_SLOT) {
                answered = model_next_slot;
                model_next_slot++;
            }
            post_event(TRB_TYPE_COMMAND_COMPLETE, TRB_COMPLETION_SUCCESS, answered,
                       (uint32_t)(uintptr_t)t);
            reg[OP(0x18u)] &= ~M_CRCR_CRR;
        }
        model_cmd_dequeue++;
        if (model_cmd_dequeue == XHCI_RING_TRBS) {
            model_cmd_dequeue = 0;
            model_cmd_cycle ^= TRB_CYCLE;
        }
    }
}

/* Where the controller has read up to on each endpoint ring, and what it
   answers a transfer with. A transfer is finished by the block that asks for an
   event; the model finds that block by walking the ring the way a controller
   does, rather than by knowing where the driver put it. */
static uint32_t model_ep_dequeue[XHCI_SLOTS + 1][XHCI_MAX_DCI];
static uint32_t model_ep_cycle[XHCI_SLOTS + 1][XHCI_MAX_DCI];
static int transfer_code;
static uint32_t transfer_residual;
static int transfer_answers;

static void endpoint_doorbell(uint32_t slot, uint32_t dci)
{
    if (slot == 0 || slot > XHCI_SLOTS || dci == 0 || dci >= XHCI_MAX_DCI) {
        return;
    }
    for (;;) {
        uint32_t at = model_ep_dequeue[slot][dci];
        const uint32_t *t = &ep_ring[slot][dci][at * TRB_WORDS];
        uint32_t type;

        if ((t[3] & TRB_CYCLE) != model_ep_cycle[slot][dci]) {
            break;
        }
        type = (t[3] >> TRB_TYPE_SHIFT) & TRB_TYPE_MASK;
        if (type == TRB_TYPE_LINK) {
            model_ep_dequeue[slot][dci] = 0;
            if (t[3] & TRB_TOGGLE) {
                model_ep_cycle[slot][dci] ^= TRB_CYCLE;
            }
            continue;
        }
        if ((t[3] & TRB_IOC) && transfer_answers) {
            uint32_t *event = &event_ring[model_event_write * TRB_WORDS];

            event[0] = (uint32_t)(uintptr_t)t;
            event[1] = 0;
            event[2] = transfer_residual | ((uint32_t)transfer_code << TRB_COMPLETION_SHIFT);
            event[3] = (TRB_TYPE_TRANSFER_EVENT << TRB_TYPE_SHIFT) | (slot << TRB_SLOT_SHIFT) |
                       (dci << 16) | model_event_cycle;
            model_event_write++;
            if (model_event_write == XHCI_RING_TRBS) {
                model_event_write = 0;
                model_event_cycle ^= TRB_CYCLE;
            }
        }
        model_ep_dequeue[slot][dci]++;
        if (model_ep_dequeue[slot][dci] == XHCI_EP_RING_TRBS) {
            model_ep_dequeue[slot][dci] = 0;
            model_ep_cycle[slot][dci] ^= TRB_CYCLE;
        }
    }
}

/* The driver points the ring somewhere after an abort; the controller reads
   from where it is told. The address written is truncated on this host, so the
   model takes the driver's own position rather than decoding it. */
static void ring_moved(void)
{
    model_cmd_dequeue = cmd_enqueue;
    model_cmd_cycle = cmd_cycle;
}

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
    starts = 1;
    command_completes = 1;
    port_reset_completes = 1;
    on_doorbell = doorbell_rung;
    on_ring_moved = ring_moved;
    on_endpoint_doorbell = endpoint_doorbell;
    transfer_code = TRB_COMPLETION_SUCCESS;
    transfer_residual = 0;
    transfer_answers = 1;
    {
        unsigned si;
        unsigned di;

        for (si = 0; si <= XHCI_SLOTS; si++) {
            for (di = 0; di < XHCI_MAX_DCI; di++) {
                model_ep_dequeue[si][di] = 0;
                model_ep_cycle[si][di] = TRB_CYCLE;
            }
        }
    }
    abort_completes = 1;
    model_cmd_dequeue = 0;
    model_cmd_cycle = TRB_CYCLE;
    model_next_slot = 1;
    model_event_write = 0;
    model_event_cycle = TRB_CYCLE;
    last_command_type = 0;
    last_command_slot = 0;
    commands_seen = 0;
    handoff_delay = 0;
    accepts_os_claim = 1;
    cnr_delay = 0;
    becomes_ready = 1;
    memory_space_enabled = 0;
    bus_master_enabled = 0;
    searched_class = 0;
    searched_subclass = 0;
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
    struct usb_port_status status;
    struct usb_command_result result;
    unsigned i;

    /* Finding the controller. */
    model_reset();
    CHECK("a controller with a memory aperture is taken", xhci_discover() == 0);
    CHECK("found by the class the standard gives USB",
          searched_class == PCI_CLASS_SERIAL_BUS && searched_subclass == PCI_SUBCLASS_USB);
    CHECK("with its registers decoded", memory_space_enabled == 1);
    CHECK("and allowed to reach memory itself", bus_master_enabled == 1);

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
    CHECK("so its capability registers can be read at all", xhci_read_caps() == 0);

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
    CHECK("the interface version is read", c->hciversion == 0x0110u);
    CHECK("the event-ring segment table limit is read", c->erst_max == 0);

    model_reset();
    reg[R_HCSPARAMS2] = (4u << 4); /* ERST Max 4 */
    CHECK("a controller with a larger segment table is read", xhci_read_caps() == 0);
    CHECK("and its limit is the field, not a guess", xhci_capabilities()->erst_max == 4);

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

    /* What must fit is the end of each register set, not its first byte. */
    model_reset();
    reg[R_DBOFF] = 0xFFF0u;
    reg[R_HCSPARAMS1] = 64u | (1u << 8) | (4u << 24);
    CHECK("a doorbell array that runs off the end is refused", xhci_read_caps() != 0);

    model_reset();
    reg[R_RTSOFF] = 0xFFE0u;
    reg[R_HCSPARAMS1] = 16u | (8u << 8) | (4u << 24);
    CHECK("interrupters that run off the end are refused", xhci_read_caps() != 0);

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

    /* What was taken from the firmware is given back, whether the driver is
       finished with it or could not use it at all. A machine whose firmware
       emulates a PS/2 keyboard out of a USB one would otherwise lose that
       keyboard to a kernel that had already given up. */
    model_reset();
    reg[XECP] = 1u | LEGSUP_BIOS_OWNED;
    reg[LEGCTL] = 0x0000E1FEu;
    CHECK("the capability list is read", xhci_read_caps() == 0);
    CHECK("the controller is taken", xhci_own() == 0);
    CHECK("its interrupts are off", (reg[LEGCTL] & 0x0000E1FEu) == 0);
    xhci_release();
    CHECK("and given back, the firmware's claim restored", (reg[XECP] & LEGSUP_OS_OWNED) == 0);
    CHECK("with its own interrupts as they were", (reg[LEGCTL] & 0x0000E1FEu) == 0x0000E1FEu);
    xhci_release();
    CHECK("giving back twice does nothing the second time", (reg[XECP] & LEGSUP_OS_OWNED) == 0);

    /* One that was never started is never written to on the way out. */
    model_reset();
    CHECK("the capability registers are read", xhci_read_caps() == 0);
    reg[OP(O_USBCMD)] = 0xABCDu;
    xhci_stop();
    CHECK("a controller that never started is not halted", reg[OP(O_USBCMD)] == 0xABCDu);

    model_reset();
    reg[XECP] = 1u | LEGSUP_BIOS_OWNED;
    accepts_os_claim = 0;
    CHECK("the capability list is read", xhci_read_caps() == 0);
    CHECK("a controller that will not take this kernel's claim is refused", xhci_own() != 0);

    /* The list belongs to the controller, so a driver may not trust it. A
       head outside the mapped window is the one malformed value a single
       entry can produce: a next pointer is a byte, so no one hop can leave a
       64 KiB aperture, and the case a walk has to survive is the circle
       below. */
    model_reset();
    reg[R_HCCPARAMS1] = 0x1u | 0x4u | (0x4000u << 16); /* a head at 64 KiB */
    CHECK("the capability list is read", xhci_read_caps() == 0);
    CHECK("a list whose head is outside the aperture is refused", xhci_own() != 0);

    model_reset();
    reg[XECP] = 3u; /* one entry, not the legacy one, ending the list */
    CHECK("the capability list is read", xhci_read_caps() == 0);
    CHECK("a list with no legacy capability leaves the controller ours", xhci_own() == 0);

    /* And a list whose first entry is inside the window but whose next step
       is not: the check has to be made on every step, not only on the head. */
    model_reset();
    reg[R_HCCPARAMS1] = 0x1u | 0x4u | ((0xFFF8u / 4u) << 16);
    reg[0xFFF8] = 3u | (0x2u << 8);
    CHECK("the capability list is read", xhci_read_caps() == 0);
    CHECK("a list whose next step leaves the aperture is refused", xhci_own() != 0);

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

    /* A controller that is not ready yet may have nothing but its own status
       written, so the driver waits before it touches the command register. */
    model_reset();
    reg[OP(O_USBSTS)] |= M_USBSTS_CNR;
    cnr_delay = 5;
    CHECK("the capability registers are read", xhci_read_caps() == 0);
    CHECK("a controller still starting is waited for", xhci_halt_reset() == 0);

    model_reset();
    reg[OP(O_USBSTS)] |= M_USBSTS_CNR;
    becomes_ready = 0;
    cnr_delay = 5;
    CHECK("the capability registers are read", xhci_read_caps() == 0);
    CHECK("a controller that is never ready is refused rather than written to",
          xhci_halt_reset() != 0);

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

    /* Starting it: the rings, the slots, and the ports powered. */
    model_reset();
    CHECK("the capability registers are read", xhci_read_caps() == 0);
    CHECK("the controller is taken", xhci_own() == 0);
    CHECK("halted and reset", xhci_halt_reset() == 0);
    CHECK("given its scratchpads", xhci_scratchpads() == 0);
    CHECK("a controller that starts is started", xhci_start() == 0);
    CHECK("with the slots this kernel can use, not every slot it offers",
          reg[OP(0x38u)] == XHCI_SLOTS);
    CHECK("its context array programmed", reg[OP(0x30u)] != 0);
    CHECK("its command ring programmed, running from the first block",
          (reg[OP(0x18u)] & ~0xFu) != 0 && (reg[OP(0x18u)] & 1u) != 0);
    CHECK("one segment in its event ring table", reg[RTSOFF + 0x20u + 0x08u] == 1);
    CHECK("the table naming the ring it built", erst[0] == (uint32_t)(uintptr_t)event_ring);
    CHECK("with the ring's length", erst[2] == XHCI_RING_TRBS);
    CHECK("and nothing in the halves this kernel cannot address",
          erst[1] == 0 && reg[RTSOFF + 0x20u + 0x14u] == 0 && reg[OP(0x34u)] == 0);
    CHECK("that table programmed", reg[RTSOFF + 0x20u + 0x10u] != 0);
    CHECK("and a dequeue pointer to start from", reg[RTSOFF + 0x20u + 0x18u] != 0);
    CHECK("it is running", (reg[OP(O_USBSTS)] & M_USBSTS_HCH) == 0);
    CHECK("and every port has power",
          (reg[M_PORTSC(0)] & M_PORTSC_PP) != 0 && (reg[M_PORTSC(3)] & M_PORTSC_PP) != 0);

    model_reset();
    starts = 0;
    CHECK("the capability registers are read", xhci_read_caps() == 0);
    CHECK("halted and reset", xhci_halt_reset() == 0);
    CHECK("a controller that will not start is refused", xhci_start() != 0);

    /* Commands. */
    model_reset();
    CHECK("the capability registers are read", xhci_read_caps() == 0);
    CHECK("halted and reset", xhci_halt_reset() == 0);
    CHECK("started", xhci_start() == 0);
    CHECK("a command is issued", xhci_command_issue(TRB_TYPE_NOOP_COMMAND, 0, 0) == 0);
    CHECK("and reaches the controller as the command it was",
          commands_seen == 1 && last_command_type == TRB_TYPE_NOOP_COMMAND);
    CHECK("it is not finished until its event has been read", xhci_command_done(&result) == 0);
    CHECK("and no second command may be issued meanwhile",
          xhci_command_issue(TRB_TYPE_NOOP_COMMAND, 0, 0) != 0);
    xhci_events();
    CHECK("once the event is read the command is finished", xhci_command_done(&result) == 1);
    CHECK("and it succeeded", result.status == USB_XFER_OK);
    CHECK("so another may be issued", xhci_command_issue(TRB_TYPE_NOOP_COMMAND, 0, 0) == 0);

    model_reset();
    command_completes = 0;
    CHECK("the capability registers are read", xhci_read_caps() == 0);
    CHECK("halted and reset", xhci_halt_reset() == 0);
    CHECK("started", xhci_start() == 0);
    CHECK("a command is issued", xhci_command_issue(TRB_TYPE_NOOP_COMMAND, 0, 0) == 0);
    CHECK("one that is never answered is still running", xhci_command_done(&result) == 0);
    xhci_advance(2000);
    CHECK("until its deadline passes", xhci_command_done(&result) == -1);
    /* The abort is a request, and the controller answers it by stopping the
       ring. Once it has, the driver may use the ring again. */
    command_completes = 1;
    CHECK("and once the controller has stopped the ring it is used again",
          xhci_command_issue(TRB_TYPE_NOOP_COMMAND, 0, 0) == 0);
    xhci_events();
    CHECK("with the next command answered", xhci_command_done(&result) == 1);

    model_reset();
    command_completes = 0;
    abort_completes = 0;
    CHECK("the capability registers are read", xhci_read_caps() == 0);
    CHECK("halted and reset", xhci_halt_reset() == 0);
    CHECK("started", xhci_start() == 0);
    CHECK("a command is issued", xhci_command_issue(TRB_TYPE_NOOP_COMMAND, 0, 0) == 0);
    xhci_advance(2000);
    CHECK("a command that is never answered is given up on", xhci_command_done(&result) == -1);
    CHECK("and while the controller still holds the ring nothing is issued on it",
          xhci_command_issue(TRB_TYPE_NOOP_COMMAND, 0, 0) != 0);

    /* A completion that names a block the driver is not waiting on belongs to
       a command already abandoned, and must not answer the one in flight. */
    model_reset();
    command_completes = 0;
    CHECK("the capability registers are read", xhci_read_caps() == 0);
    CHECK("halted and reset", xhci_halt_reset() == 0);
    CHECK("started", xhci_start() == 0);
    CHECK("a command is issued", xhci_command_issue(TRB_TYPE_NOOP_COMMAND, 0, 0) == 0);
    post_event(TRB_TYPE_COMMAND_COMPLETE, TRB_COMPLETION_SUCCESS, 0, 0xDEADBEEFu);
    xhci_events();
    CHECK("a completion for another command does not finish this one",
          xhci_command_done(&result) == 0);

    /* The command ring wraps through the link block at its end, and both
       sides change cycle there. */
    model_reset();
    CHECK("the capability registers are read", xhci_read_caps() == 0);
    CHECK("halted and reset", xhci_halt_reset() == 0);
    CHECK("started", xhci_start() == 0);
    for (i = 0; i < XHCI_RING_TRBS - 1; i++) {
        xhci_command_issue(TRB_TYPE_NOOP_COMMAND, 0, 0);
        xhci_events();
        xhci_command_done(&result);
    }
    CHECK("the ring's last block is a link back to its start",
          ((cmd_ring[(XHCI_RING_TRBS - 1) * TRB_WORDS + 3] >> TRB_TYPE_SHIFT) & TRB_TYPE_MASK) ==
              TRB_TYPE_LINK);
    CHECK("with the bit that changes the cycle there",
          (cmd_ring[(XHCI_RING_TRBS - 1) * TRB_WORDS + 3] & TRB_TOGGLE) != 0);
    CHECK("and the driver's own cycle changed with it", cmd_cycle == 0);
    CHECK("so the next command goes to the first block", cmd_enqueue == 0);
    CHECK("a command after the wrap is issued",
          xhci_command_issue(TRB_TYPE_NOOP_COMMAND, 0, 0) == 0);
    CHECK("with the new cycle", (cmd_ring[3] & TRB_CYCLE) == 0);

    /* The event ring wraps too, and the driver has to follow the controller
       through it rather than stopping at the end. */
    model_reset();
    CHECK("the capability registers are read", xhci_read_caps() == 0);
    CHECK("halted and reset", xhci_halt_reset() == 0);
    CHECK("started", xhci_start() == 0);
    for (i = 0; i < XHCI_RING_TRBS + 40; i++) {
        post_event(TRB_TYPE_PORT_CHANGE, TRB_COMPLETION_SUCCESS, 0, 1u << 24);
        if ((i % 64) == 0) {
            xhci_events();
        }
    }
    xhci_events();
    CHECK("every event is taken, wrap and all", event_index == model_event_write);
    CHECK("and the driver's cycle follows the controller's", event_cycle == model_event_cycle);
    CHECK("with the dequeue pointer written back", (reg[RTSOFF + 0x20u + 0x18u] & 0x8u) != 0);
    CHECK("and the port it named remembered", port_changed[0] == 1);
    CHECK("with the dequeue pointer at the block it stopped on",
          (reg[RTSOFF + 0x20u + 0x18u] & ~0xFu) ==
              ((uint32_t)(uintptr_t)&event_ring[event_index * TRB_WORDS] & ~0xFu));

    /* Ports. */
    model_reset();
    CHECK("the capability registers are read", xhci_read_caps() == 0);
    CHECK("halted and reset", xhci_halt_reset() == 0);
    CHECK("started", xhci_start() == 0);
    reg[M_PORTSC(0)] |= M_PORTSC_CCS | (3u << 10);
    CHECK("a port with a device says so", xhci_port_status(0, &status) == 0);
    CHECK("with the speed the controller reports", status.connected == 1 && status.speed == 3);
    CHECK("a port with nothing on it says that", xhci_port_status(1, &status) == 0);
    CHECK("and is not connected", status.connected == 0);
    CHECK("a port outside the controller's own count is refused",
          xhci_port_status(99, &status) != 0);

    CHECK("a port with a device is reset", xhci_port_reset_begin(0) == 0);
    CHECK("and the reset finished", xhci_port_reset_done(0) == 1);
    CHECK("with its change bit cleared afterwards", (reg[M_PORTSC(0)] & M_PORTSC_PRC) == 0);
    CHECK("a port with nothing on it is not reset", xhci_port_reset_begin(1) != 0);

    /* A contact bounces, so a connect is not acted on the moment it appears. */
    model_reset();
    CHECK("the capability registers are read", xhci_read_caps() == 0);
    CHECK("halted and reset", xhci_halt_reset() == 0);
    CHECK("started", xhci_start() == 0);
    reg[M_PORTSC(0)] |= M_PORTSC_CCS;
    xhci_events();
    CHECK("a connect is not settled the moment it appears", xhci_port_settled(0) == 0);
    xhci_advance(50);
    CHECK("nor half way through the interval", xhci_port_settled(0) == 0);
    xhci_advance(60);
    CHECK("and is settled once the interval has passed", xhci_port_settled(0) == 1);
    reg[M_PORTSC(0)] &= ~M_PORTSC_CCS;
    xhci_events();
    CHECK("a port with nothing on it is never settled", xhci_port_settled(0) == 0);

    /* A controller with more ports than this kernel has state for. */
    model_reset();
    reg[R_HCSPARAMS1] = 16u | (1u << 8) | (40u << 24);
    CHECK("a controller with more ports than this kernel reaches is read", xhci_read_caps() == 0);
    CHECK("and reaches as many as it has state for", xhci_capabilities()->max_ports == 16);

    model_reset();
    port_reset_completes = 0;
    CHECK("the capability registers are read", xhci_read_caps() == 0);
    CHECK("halted and reset", xhci_halt_reset() == 0);
    CHECK("started", xhci_start() == 0);
    reg[M_PORTSC(0)] |= M_PORTSC_CCS;
    CHECK("a reset is begun", xhci_port_reset_begin(0) == 0);
    CHECK("and is not finished", xhci_port_reset_done(0) == 0);
    xhci_advance(1000);
    CHECK("a reset that never finishes is given up on", xhci_port_reset_done(0) == -1);

    /* The write discipline this register needs, which is the easiest thing in
       the driver to get wrong: half its bits are cleared by writing them. */
    model_reset();
    CHECK("the capability registers are read", xhci_read_caps() == 0);
    CHECK("halted and reset", xhci_halt_reset() == 0);
    CHECK("started", xhci_start() == 0);
    port_reset_completes = 0;
    reg[M_PORTSC(0)] |= M_PORTSC_CCS | M_PORTSC_PED | M_PORTSC_CSC;
    CHECK("a reset is begun", xhci_port_reset_begin(0) == 0);
    CHECK("without throwing away a connect nobody has handled yet",
          (reg[M_PORTSC(0)] & M_PORTSC_CSC) != 0);
    CHECK("and without disabling the port it is resetting", (reg[M_PORTSC(0)] & M_PORTSC_PED) != 0);

    /* Devices, endpoints and the recovery a halted one needs. */
    model_reset();
    CHECK("the capability registers are read", xhci_read_caps() == 0);
    CHECK("halted and reset", xhci_halt_reset() == 0);
    CHECK("started", xhci_start() == 0);
    reg[M_PORTSC(0)] |= M_PORTSC_CCS | (3u << 10);
    CHECK("a slot is asked for", xhci_device_open(0, USB_SPEED_HIGH) == 0);
    CHECK("with the command that asks", last_command_type == TRB_TYPE_ENABLE_SLOT);
    xhci_events();
    CHECK("and the controller answers with one", xhci_command_done(&result) == 1);
    CHECK("which is the device handle", result.device == 1);
    CHECK("the device is addressed", xhci_device_address(1) == 0);
    CHECK("with the command that addresses it", last_command_type == TRB_TYPE_ADDRESS_DEVICE);
    CHECK("naming the slot", last_command_slot == 1);
    CHECK("its context is where the controller will look", dcbaa[1] != 0);
    xhci_events();
    CHECK("the controller answers", xhci_command_done(&result) == 1);

    {
        struct usb_endpoint ep;

        ep.number = 1;
        ep.direction = USB_DIR_IN;
        ep.type = USB_EP_INTERRUPT;
        ep.max_packet = 8;
        ep.interval = 10;
        CHECK("an endpoint is opened", xhci_endpoint_open(1, &ep) == 0);
        CHECK("with the command that configures one", last_command_type == TRB_TYPE_CONFIG_EP);
        xhci_events();
        CHECK("the controller answers", xhci_command_done(&result) == 1);
    }

    /* A halted endpoint is reset and its ring pointed at the block the driver
       will write next. Returning an error and leaving the ring alone would
       leave the endpoint unusable for the rest of the boot. */
    CHECK("a halted endpoint is reset", xhci_endpoint_reset(1, 3) == 0);
    CHECK("with the command that resets one", last_command_type == TRB_TYPE_RESET_EP);
    xhci_events();
    CHECK("the controller answers", xhci_command_done(&result) == 1);
    CHECK("and its ring is pointed somewhere again", xhci_endpoint_dequeue(1, 3) == 0);
    CHECK("with the command that points it", last_command_type == TRB_TYPE_SET_TR_DEQUEUE);
    xhci_events();
    CHECK("the controller answers", xhci_command_done(&result) == 1);

    CHECK("a full-speed device's control endpoint is corrected once it is known",
          xhci_device_packet_size(1, 64) == 0);
    CHECK("with the command that changes one field of a context",
          last_command_type == TRB_TYPE_EVALUATE_CTX);
    xhci_events();
    CHECK("the controller answers", xhci_command_done(&result) == 1);

    CHECK("a device the controller never gave out is refused", xhci_device_address(3) != 0);
    /* Slots are numbered from one, so the last one this kernel asked for is
       `XHCI_SLOTS` and not one less. A driver whose tables were a slot short
       would refuse the last slot the controller is entitled to hand out. */
    CHECK("the last slot the controller may give out is not refused for being last",
          xhci_device_address(XHCI_SLOTS) != 0 && slots[XHCI_SLOTS].used == 0);
    slots[XHCI_SLOTS].used = 1;
    slots[XHCI_SLOTS].port = 0;
    slots[XHCI_SLOTS].speed = USB_SPEED_HIGH;
    CHECK("and is addressed like any other", xhci_device_address(XHCI_SLOTS) == 0);
    xhci_events();
    CHECK("the controller answers", xhci_command_done(&result) == 1);
    CHECK("and so is an endpoint on one", xhci_endpoint_reset(3, 3) != 0);

    /* Transfers. The control transfer is three blocks and one event, and the
       event is asked for on the block that says how much arrived. */
    {
        struct usb_transfer transfer;
        struct usb_completion completion;
        static uint8_t setup[8];
        static uint8_t buffer[64];
        const uint32_t *ring = &ep_ring[1][1][0];

        model_reset();
        CHECK("the capability registers are read", xhci_read_caps() == 0);
        CHECK("halted and reset", xhci_halt_reset() == 0);
        CHECK("started", xhci_start() == 0);
        reg[M_PORTSC(0)] |= M_PORTSC_CCS | (3u << 10);
        CHECK("a slot is asked for", xhci_device_open(0, USB_SPEED_HIGH) == 0);
        xhci_events();
        CHECK("the controller gives one", xhci_command_done(&result) == 1);
        CHECK("the device is addressed", xhci_device_address(1) == 0);
        xhci_events();
        CHECK("the controller answers", xhci_command_done(&result) == 1);

        setup[0] = 0x80;
        setup[1] = 6;
        transfer.setup = setup;
        transfer.buffer = buffer;
        transfer.length = 18;
        transfer.direction = USB_DIR_IN;
        CHECK("a control transfer is queued", xhci_submit(1, 1, &transfer) == 0);
        CHECK("as a setup block", ((ring[3] >> TRB_TYPE_SHIFT) & TRB_TYPE_MASK) == TRB_TYPE_SETUP);
        CHECK("carrying the eight bytes itself", (ring[3] & TRB_IDT) != 0);
        CHECK("saying which way the data goes", (ring[3] & (3u << 16)) == TRB_TRT_IN);
        CHECK("then a data block",
              ((ring[TRB_WORDS + 3] >> TRB_TYPE_SHIFT) & TRB_TYPE_MASK) == TRB_TYPE_DATA);
        CHECK("which asks for the event, because it is the one that says how much came",
              (ring[TRB_WORDS + 3] & TRB_IOC) != 0);
        CHECK("and a status block the other way",
              ((ring[2 * TRB_WORDS + 3] >> TRB_TYPE_SHIFT) & TRB_TYPE_MASK) == TRB_TYPE_STATUS);
        xhci_events();
        CHECK("the transfer is reported", xhci_poll(&completion) == 1);
        CHECK("for the device and endpoint it was on",
              completion.device == 1 && completion.endpoint == 1);
        CHECK("as having moved everything", completion.status == USB_XFER_OK);
        CHECK("with the length it asked for", completion.length == 18);
        CHECK("and nothing missing", completion.residual == 0);
        CHECK("and there is nothing else waiting", xhci_poll(&completion) == 0);

        /* A device that answers with less than was asked for. */
        transfer_code = TRB_COMPLETION_SHORT;
        transfer_residual = 6;
        CHECK("a second transfer is queued", xhci_submit(1, 1, &transfer) == 0);
        xhci_events();
        CHECK("a short answer is reported", xhci_poll(&completion) == 1);
        CHECK("as short", completion.status == USB_XFER_SHORT);
        CHECK("with what did not arrive", completion.residual == 6);
        CHECK("against what was asked for", completion.length == 18);

        /* A halted endpoint. */
        transfer_code = TRB_COMPLETION_STALL;
        transfer_residual = 0;
        CHECK("a third transfer is queued", xhci_submit(1, 1, &transfer) == 0);
        xhci_events();
        CHECK("a stall is reported", xhci_poll(&completion) == 1);
        CHECK("as a stall, which is the one a caller recovers from",
              completion.status == USB_XFER_STALLED);

        /* A transfer the controller never answers. */
        transfer_answers = 0;
        CHECK("a fourth transfer is queued", xhci_submit(1, 1, &transfer) == 0);
        xhci_events();
        CHECK("and nothing is reported for it", xhci_poll(&completion) == 0);
        transfer_answers = 1;

        /* An interrupt endpoint, which carries one block and no setup. */
        {
            struct usb_endpoint ep;

            ep.number = 1;
            ep.direction = USB_DIR_IN;
            ep.type = USB_EP_INTERRUPT;
            ep.max_packet = 8;
            ep.interval = 10;
            CHECK("an endpoint is opened", xhci_endpoint_open(1, &ep) == 0);
            xhci_events();
            CHECK("the controller answers", xhci_command_done(&result) == 1);
        }
        transfer.setup = 0;
        transfer.length = 8;
        CHECK("a report is queued on it", xhci_submit(1, 3, &transfer) == 0);
        CHECK("as one ordinary block",
              ((ep_ring[1][3][3] >> TRB_TYPE_SHIFT) & TRB_TYPE_MASK) == TRB_TYPE_NORMAL);
        xhci_events();
        CHECK("and is reported", xhci_poll(&completion) == 1);
        CHECK("on the endpoint it was queued on", completion.endpoint == 3);

        /* A device that has gone, answering for a transfer nobody waits for. */
        CHECK("a report is queued", xhci_submit(1, 3, &transfer) == 0);
        slots[1].used = 0;
        xhci_events();
        CHECK("a completion for a slot the controller no longer holds is dropped",
              xhci_poll(&completion) == 0);
    }

    /* The interval the controller schedules from is derived from the one the
       device asked for, and the two are neither the same unit nor the same
       scale. A driver that copied the number would poll a keyboard sixty-four
       times too often or too seldom. */
    CHECK("a full-speed device asking for one millisecond", interval_for(USB_SPEED_FULL, 1) == 3);
    CHECK("a full-speed device asking for ten", interval_for(USB_SPEED_FULL, 10) == 6);
    CHECK("a low-speed device asking for ten", interval_for(USB_SPEED_LOW, 10) == 6);
    CHECK("a high-speed device asking for one", interval_for(USB_SPEED_HIGH, 1) == 0);
    CHECK("a high-speed device asking for eight", interval_for(USB_SPEED_HIGH, 8) == 7);
    CHECK("a device asking for nothing is given the smallest interval",
          interval_for(USB_SPEED_FULL, 0) == 3);
    CHECK("and one asking for more than the field holds is clamped",
          interval_for(USB_SPEED_HIGH, 200) == 15);

    /* The size of a control endpoint before the device has been read. */
    CHECK("a low-speed device's control endpoint", default_packet_size(USB_SPEED_LOW) == 8);
    CHECK("a full-speed device's, until it says otherwise",
          default_packet_size(USB_SPEED_FULL) == 8);
    CHECK("a high-speed device's", default_packet_size(USB_SPEED_HIGH) == 64);
    CHECK("a SuperSpeed device's", default_packet_size(USB_SPEED_SUPER) == 512);

    /* The device context index of an endpoint, which is what a doorbell
       names. */
    {
        struct usb_endpoint ep;

        ep.number = 0;
        ep.direction = USB_DIR_IN;
        ep.type = USB_EP_CONTROL;
        ep.max_packet = 8;
        ep.interval = 0;
        CHECK("endpoint zero is index one", dci_of(&ep) == 1);
        ep.number = 1;
        ep.direction = USB_DIR_IN;
        CHECK("an in endpoint is odd", dci_of(&ep) == 3);
        ep.direction = USB_DIR_OUT;
        CHECK("an out endpoint is even", dci_of(&ep) == 2);
        ep.number = 9;
        CHECK("an endpoint beyond this kernel's table is refused", dci_of(&ep) == 0);
    }

    if (fails == 0) {
        printf("xhci_hosttest: OK\n");
        return 0;
    }
    printf("xhci_hosttest: %d failures\n", fails);
    return 1;
}
