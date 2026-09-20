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
extern int strcmp(const char *left, const char *right);

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
#define M_USBCMD_INTE  0x00000004u
#define M_USBSTS_HCH   0x1u
#define M_USBSTS_CNR   0x800u
#define M_USBSTS_EINT  0x8u

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
static uint32_t delayed_us;
static uint32_t halt_after_us;
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

static uint32_t model_r32(struct xhci_ctrl *hc, uint32_t off)
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

/*
 * The interrupter, as a controller works it: an event sets the pending bit and
 * the busy bit, and raises an interrupt only when the busy bit was clear and
 * both enables are set. The busy bit is cleared by the driver writing it back
 * as one, which is what makes a driver that writes it as zero stop hearing
 * from its controller after the first event. Nothing else here models that,
 * and no machine can be made to show it on demand.
 */
#define IR_BASE    (RTSOFF + 0x20u)
#define M_IMAN     (IR_BASE + 0x00u)
#define M_IMOD     (IR_BASE + 0x04u)
#define M_ERDP     (IR_BASE + 0x18u)
#define M_IMAN_IP  0x1u
#define M_IMAN_IE  0x2u
#define M_ERDP_EHB 0x8u

static int model_busy;    /* the event-handler-busy bit */
static int model_raises;  /* interrupts the controller has raised */
static int msi_vector;    /* the vector its capability was given, or -1 */
static int msi_refuses;   /* the device has no MSI capability */
static int model_irq;     /* the line the firmware routed, in configuration space */
static int intx_enabled;  /* the command register's interrupt disable bit cleared */
static int event_on_iman; /* post an event from inside the acknowledgement */

static void post_event(uint32_t type, uint32_t code, uint32_t slot, uint32_t param0);

/** What the controller does when it has written an event. */
static void model_raise(void)
{
    int was_busy = model_busy;

    model_busy = 1;
    reg[M_IMAN] |= M_IMAN_IP;
    reg[OP(O_USBSTS)] |= M_USBSTS_EINT;
    if (was_busy) {
        return; /* the driver has not said it is finished with the last one */
    }
    if (!(reg[M_IMAN] & M_IMAN_IE)) {
        return;
    }
    if (!(reg[OP(O_USBCMD)] & M_USBCMD_INTE)) {
        return;
    }
    if (msi_vector < 0 && !intx_enabled) {
        return;
    }
    model_raises++;
}

static void model_w32(struct xhci_ctrl *hc, uint32_t off, uint32_t v)
{
    if (off >= REGS) {
        return;
    }

    if (off == M_IMAN) {
        uint32_t pending = reg[off] & M_IMAN_IP;

        if (v & M_IMAN_IP) {
            pending = 0; /* the pending bit is cleared by writing it as one */
        }
        reg[off] = pending | (v & M_IMAN_IE);
        if (event_on_iman) {
            /* An event written between the acknowledgement and the drain,
               which is the window a handler that drained first would lose.
               The numbers are the port-change event's, spelled here because
               the driver's own names are defined further down this file. */
            event_on_iman = 0;
            post_event(34u, 1u, 0, 1u << 24);
        }
        return;
    }

    if (off == OP(O_USBSTS)) {
        /* The event bit is cleared by writing it as one; the rest of the
           register is the controller's to set. */
        reg[off] &= ~(v & M_USBSTS_EINT);
        return;
    }

    if (off == M_ERDP) {
        if (v & M_ERDP_EHB) {
            model_busy = 0; /* the driver is finished with what it was given */
        }
        reg[off] = v;
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
    delayed_us += us;
    if (halt_after_us && delayed_us >= halt_after_us) {
        reg[OP(O_USBSTS)] |= M_USBSTS_HCH;
    }
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
/* The controller's configuration space, which is where a chipset keeps the
   routing of its USB pins, and the maker it says it is. */
static uint32_t pci_cfg[256];
static uint16_t model_vendor;
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
    out->vendor_id = model_vendor;
    out->device_id = 0x9c31;
    out->bus = 0;
    out->device = 20;
    out->function = 0;
    out->prog_if = model_progif;
    out->bar[0] = model_bar0;
    out->bar[1] = model_bar1;
    out->irq = (uint8_t)model_irq;
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

int pci_msi_enable(const pci_device_t *dev, uint8_t vector)
{
    (void)dev;
    if (msi_refuses) {
        return -1;
    }
    msi_vector = (int)vector;
    return 0;
}

void pci_msi_disable(const pci_device_t *dev)
{
    (void)dev;
    msi_vector = -1;
}

void pci_enable_interrupts(pci_device_t *dev)
{
    (void)dev;
    intx_enabled = 1;
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
    return pci_cfg[off];
}

void pci_write_config32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off, uint32_t v)
{
    (void)bus;
    (void)dev;
    (void)fn;
    pci_cfg[off] = v;
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

static int stop_halted;
static int stop_timeout;

void klog(int level, const char *subsystem, const char *format, ...)
{
    (void)level;
    if (strcmp(subsystem, "usb") == 0) {
        if (strcmp(format, "stop: the controller is halted") == 0) {
            stop_halted++;
        }
        if (strcmp(format, "stop: the controller would not halt") == 0) {
            stop_timeout++;
        }
    }
}

/*
 * The kernel heap, as this test supplies it: an arena of pages with a bitmap,
 * and a bump for the plain allocations. A page here is a page of this
 * program's memory and its "physical" address is the address itself, which is
 * what the driver programs into the model's registers and what the model
 * reads back.
 */
#define ARENA_PAGES 1024
static uint8_t arena[ARENA_PAGES][4096] __attribute__((aligned(4096)));
static uint8_t arena_taken[ARENA_PAGES];
static uint8_t bump[256 * 1024];
static unsigned bump_used;
static unsigned bump_live;

/* The physical address of a page the driver reaches by another one. A row
   that wants a buffer's pages out of order sets the map; the ordinary answer
   is the address itself, as the identity window's is. */
static uintptr_t (*virt_map)(uintptr_t va);
uintptr_t paging_virt_to_phys(uintptr_t va)
{
    return virt_map ? virt_map(va) : va;
}

void *kalloc_pages(uint32_t count, uintptr_t *phys)
{
    unsigned i;

    if (count != 1) {
        return 0; /* this driver takes its pages one at a time */
    }
    for (i = 0; i < ARENA_PAGES; i++) {
        if (!arena_taken[i]) {
            unsigned j;

            arena_taken[i] = 1;
            for (j = 0; j < 4096; j++) {
                arena[i][j] = 0;
            }
            if (phys) {
                *phys = (uintptr_t)&arena[i][0];
            }
            return &arena[i][0];
        }
    }
    return 0;
}

void kfree_pages(void *p, uint32_t count)
{
    unsigned i;

    (void)count;
    for (i = 0; i < ARENA_PAGES; i++) {
        if (p == &arena[i][0]) {
            arena_taken[i] = 0;
            return;
        }
    }
}

void *kzalloc(size_t size)
{
    unsigned char *p;
    size_t i;

    size = (size + 15u) & ~(size_t)15u;
    if (bump_used + size > sizeof(bump)) {
        return 0;
    }
    p = &bump[bump_used];
    bump_used += (unsigned)size;
    bump_live++;
    for (i = 0; i < size; i++) {
        p[i] = 0;
    }
    return p;
}

void *kmalloc(size_t size)
{
    return kzalloc(size);
}

void kfree(void *p)
{
    /* The driver gives every one of these back before it takes them again,
     * so the bump starts over once nothing is out. */
    if (p && bump_live > 0 && --bump_live == 0) {
        bump_used = 0;
    }
}

/* Two of the kernel's own, which the driver reaches and this test does not
 * build: a fill, and the name of a controller's register interface. */
void *k_memset(void *ptr, int c, size_t n)
{
    unsigned char *p = (unsigned char *)ptr;

    while (n-- > 0) {
        *p++ = (unsigned char)c;
    }
    return ptr;
}

const char *usb_interface_name(uint8_t prog_if)
{
    switch (prog_if) {
    case 0x00:
        return "uhci";
    case 0x10:
        return "ohci";
    case 0x20:
        return "ehci";
    case 0x30:
        return "xhci";
    default:
        return "other";
    }
}

/* The driver's barrier, watched: a row that checks how a transfer of several
   blocks is published looks at the ring at the first barrier of a submission,
   when every block but the first must be visible and the first not. */
static int watch_publish;
static uint32_t watch_first; /* the ring index of the first block */
static uint32_t watch_cycle; /* the producer's cycle bit at submission */
static const uint32_t *watch_ring;
static int watch_result;      /* -1 not looked, 1 first invalid and the rest valid, 0 otherwise */
static unsigned watch_blocks; /* how many blocks the watched transfer has */
static void model_barrier(void);
#define xhci_barrier() model_barrier()
/* The host build has no identity-mapped window: a buffer's address is
   whatever the loader gave it, so every one goes through the map. The
   driver takes an address below the window's end as its own physical
   address, which on the host depended on the load address of this program
   and made the map rows fail on one run in two. */
#include "../../rach2/sys/mm/paging.h"
#undef HARD_MEMORY_LIMIT
#define HARD_MEMORY_LIMIT 0u
#include "../../rach2/sys/usb/xhci.c"

static void model_barrier(void)
{
    if (watch_publish && watch_result < 0) {
        /* At the first barrier the first block is not yet valid and every
           block after it is, the Link included, with the cycle the
           producer has on each side of the Link. */
        uint32_t pos = watch_first;
        uint32_t cycle = watch_cycle;
        unsigned i;
        int ok = 1;

        for (i = 0; i < watch_blocks; i++) {
            uint32_t c;

            if (pos == XHCI_EP_RING_TRBS - 1) {
                c = watch_ring[pos * TRB_WORDS + 3] & TRB_CYCLE;
                ok = ok && c == cycle;
                pos = 0;
                cycle ^= TRB_CYCLE;
            }
            c = watch_ring[pos * TRB_WORDS + 3] & TRB_CYCLE;
            ok = ok && (i == 0 ? c != cycle : c == cycle);
            pos++;
        }
        watch_result = ok;
    }
}

/*
 * The controller under test. Discovery fills the driver's table; every call
 * after it names the one controller this model presents.
 */
static struct xhci_ctrl *hc;

static int discover(void)
{
    int rc = xhci_discover();

    hc = xhci_controller(0);
    return rc;
}

/* ---- what the controller does with what the driver gave it ---- */

/* Where the model writes its next event, and with which cycle value. It writes
   into the driver's own event ring, which is the memory the driver handed the
   controller, reached here by name rather than through the truncated address
   the driver wrote into `ERSTBA`. */
static uint32_t model_event_write;
static uint32_t model_event_cycle;

static void post_event(uint32_t type, uint32_t code, uint32_t slot, uint32_t param0)
{
    uint32_t *t = &hc->event_ring[model_event_write * TRB_WORDS];

    t[0] = param0;
    t[1] = 0;
    t[2] = code << TRB_COMPLETION_SHIFT;
    t[3] = (type << TRB_TYPE_SHIFT) | (slot << TRB_SLOT_SHIFT) | model_event_cycle;
    model_event_write++;
    if (model_event_write == XHCI_RING_TRBS) {
        model_event_write = 0;
        model_event_cycle ^= TRB_CYCLE;
    }
    model_raise();
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

static uint32_t model_ep_dequeue[XHCI_SLOTS + 1][XHCI_MAX_DCI];
static uint32_t model_ep_cycle[XHCI_SLOTS + 1][XHCI_MAX_DCI];
/* What a bulk transfer moves, and what the controller does to one: the
   medium the blocks read from or write to, the direction of each endpoint,
   a short packet at block N of a transfer with the bytes it leaves, whether
   the last block's event follows a short as the specification says or not as
   QEMU does, a failure at block N with its code and residual, and the state
   a failure or a stop leaves the endpoint in. The blocks fetched are kept
   so a row can require which were executed. */
static uint8_t medium[1024 * 1024];
static uint32_t medium_at;
static int model_ep_in[XHCI_SLOTS + 1][XHCI_MAX_DCI];
static int model_ep_halted[XHCI_SLOTS + 1][XHCI_MAX_DCI];
static int model_ep_error[XHCI_SLOTS + 1]
                         [XHCI_MAX_DCI]; /* the Error state: a block it could not run */
static int model_td_pos;
static int short_at;
static uint32_t short_left;
static int terminal_after_short;
static int fail_at;
static uint32_t fail_code;
static uint32_t fail_left;
static uint32_t stop_events;
static int command_answer_code;
static const uint32_t *model_fetched[64];
static unsigned model_fetched_n;

static void post_transfer(uint32_t code, uint32_t residual, uint32_t slot, uint32_t dci,
                          const uint32_t *t)
{
    uint32_t *event = &hc->event_ring[model_event_write * TRB_WORDS];

    event[0] = (uint32_t)(uintptr_t)t;
    event[1] = 0;
    event[2] = residual | (code << TRB_COMPLETION_SHIFT);
    event[3] = (TRB_TYPE_TRANSFER_EVENT << TRB_TYPE_SHIFT) | (slot << TRB_SLOT_SHIFT) |
               (dci << 16) | model_event_cycle;
    model_event_write++;
    if (model_event_write == XHCI_RING_TRBS) {
        model_event_write = 0;
        model_event_cycle ^= TRB_CYCLE;
    }
}

static void doorbell_rung(uint32_t target)
{
    if (target != 0) {
        return;
    }

    for (;;) {
        const uint32_t *t = &hc->cmd_ring[model_cmd_dequeue * TRB_WORDS];
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
            uint32_t code = TRB_COMPLETION_SUCCESS;
            uint32_t edci = (t[3] >> 16) & 0x1Fu;

            if (last_command_type == TRB_TYPE_ENABLE_SLOT) {
                answered = model_next_slot;
                model_next_slot++;
            }
            /* The endpoint commands, with the preconditions a controller
               enforces: a halted endpoint is not stopped, a running one is
               not reset (xHCI 1.2, 4.6.9, 4.6.8). A stop posts the Stopped
               event for the block the endpoint was on, before its own
               completion; a dequeue moves the controller's pointer to the
               address and cycle the command carries. */
            if (last_command_type == TRB_TYPE_STOP_EP && answered <= XHCI_SLOTS &&
                edci < XHCI_MAX_DCI) {
                if (model_ep_halted[answered][edci] || model_ep_error[answered][edci]) {
                    code = TRB_COMPLETION_CTX_STATE;
                } else if (hc->slots[answered].ring[edci]) {
                    const uint32_t *at =
                        &hc->slots[answered]
                             .ring[edci][model_ep_dequeue[answered][edci] * TRB_WORDS];

                    post_transfer(TRB_COMPLETION_STOPPED, 0, answered, edci, at);
                    stop_events++;
                }
            } else if (last_command_type == TRB_TYPE_RESET_EP && answered <= XHCI_SLOTS &&
                       edci < XHCI_MAX_DCI) {
                if (!model_ep_halted[answered][edci]) {
                    code = TRB_COMPLETION_CTX_STATE;
                } else {
                    model_ep_halted[answered][edci] = 0;
                }
            } else if (last_command_type == TRB_TYPE_SET_TR_DEQUEUE && answered <= XHCI_SLOTS &&
                       edci < XHCI_MAX_DCI && hc->slots[answered].ring[edci]) {
                uintptr_t ring = (uintptr_t)hc->slots[answered].ring[edci];
                uintptr_t where =
                    (uintptr_t)(((uint64_t)t[0] | ((uint64_t)t[1] << 32)) & ~(uint64_t)0xFu);

                if (where >= ring && where < ring + XHCI_EP_RING_TRBS * TRB_WORDS * 4u) {
                    model_ep_dequeue[answered][edci] =
                        (uint32_t)((where - ring) / (TRB_WORDS * 4u));
                    model_ep_cycle[answered][edci] = t[0] & TRB_CYCLE;
                    model_ep_error[answered][edci] = 0; /* Error to Stopped (4.6.10) */
                }
            }
            if (command_answer_code != 0) {
                code = (uint32_t)command_answer_code;
                command_answer_code = 0;
            }
            post_event(TRB_TYPE_COMMAND_COMPLETE, code, answered, (uint32_t)(uintptr_t)t);
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
static int transfer_code;
static uint32_t transfer_residual;
static int transfer_answers;
static int status_code; /* when set, the code the next status stage's event carries */
/* The controller's step past one block of `slot`'s `dci` ring, Links
   followed. */
static void model_ep_advance(uint32_t slot, uint32_t dci)
{
    model_ep_dequeue[slot][dci]++;
    if (model_ep_dequeue[slot][dci] == XHCI_EP_RING_TRBS) {
        model_ep_dequeue[slot][dci] = 0;
        model_ep_cycle[slot][dci] ^= TRB_CYCLE;
    }
}

/* The driver's addresses are 32 bits wide, the kernel's, and a block names
   a page by the low word of the host address the test gave it; a row that
   moves data registers its pages here so the model finds them again. A
   block naming no registered page moves nothing. */
/* A 512 KiB transfer's 129 pieces, and pages beyond them for a refusal. */
#define BIG_PAGES 260 /* a 512 KiB transfer's 129 pieces, and more for a refusal */
static uint8_t *model_pages[BIG_PAGES + 64];
static unsigned model_pages_n;
static void model_page_register(void *p)
{
    if (model_pages_n < BIG_PAGES + 64) {
        model_pages[model_pages_n++] = (uint8_t *)p;
    }
}
static uint8_t *model_page_lookup(uint32_t lo)
{
    unsigned i;

    for (i = 0; i < model_pages_n; i++) {
        if ((uint32_t)(uintptr_t)model_pages[i] == (lo & ~0xFFFu)) {
            return model_pages[i] + (lo & 0xFFFu);
        }
    }
    return 0;
}

static void model_move(uint32_t slot, uint32_t dci, const uint32_t *t, uint32_t bytes)
{
    uint8_t *page = model_page_lookup(t[0]);
    uint32_t i;

    if (!page) {
        medium_at += bytes;
        return;
    }
    for (i = 0; i < bytes && medium_at + i < sizeof(medium); i++) {
        if (model_ep_in[slot][dci]) {
            page[i] = medium[medium_at + i];
        } else {
            medium[medium_at + i] = page[i];
        }
    }
    medium_at += bytes;
}

static void endpoint_doorbell(uint32_t slot, uint32_t dci)
{
    if (slot == 0 || slot > XHCI_SLOTS || dci == 0 || dci >= XHCI_MAX_DCI ||
        !hc->slots[slot].ring[dci]) {
        return; /* no ring has been given to that endpoint */
    }
    for (;;) {
        uint32_t at = model_ep_dequeue[slot][dci];
        const uint32_t *t = &hc->slots[slot].ring[dci][at * TRB_WORDS];
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
        if (model_ep_halted[slot][dci] || model_ep_error[slot][dci]) {
            break; /* a halted or errored endpoint fetches nothing until it is moved on */
        }
        if (model_fetched_n < 64) {
            model_fetched[model_fetched_n++] = t;
        }
        model_td_pos = (model_td_pos > 0 && (model_fetched_n > 1) &&
                        (model_fetched[model_fetched_n - 2][3] & TRB_CHAIN))
                           ? model_td_pos + 1
                           : 1;
        if (type == TRB_TYPE_NORMAL) {
            uint32_t bytes = t[2] & 0x1FFFFu;

            if (fail_at == model_td_pos) {
                /* The block fails: its event, and the endpoint halts there,
                   or enters the Error state for a block the controller
                   could not run (xHCI 1.2, 4.8.3). */
                fail_at = 0;
                post_transfer(fail_code, fail_left, slot, dci, t);
                if (fail_code == TRB_COMPLETION_STALL || fail_code == TRB_COMPLETION_TXN_ERROR ||
                    fail_code == TRB_COMPLETION_BABBLE || fail_code == TRB_COMPLETION_SPLIT_ERROR) {
                    model_ep_halted[slot][dci] = 1;
                } else {
                    model_ep_error[slot][dci] = 1;
                }
                model_ep_advance(slot, dci);
                break;
            }
            if (short_at == model_td_pos) {
                const uint32_t *last = t;

                short_at = 0;
                model_move(slot, dci, t, bytes > short_left ? bytes - short_left : 0);
                if (t[3] & (TRB_ISP | TRB_IOC)) {
                    post_transfer(TRB_COMPLETION_SHORT, short_left, slot, dci, t);
                }
                /* The rest of the transfer is skipped, Links followed, to the
                   block without a chain bit; its event is posted when the
                   controller is one that does. */
                while (last[3] & TRB_CHAIN) {
                    model_ep_advance(slot, dci);
                    last = &hc->slots[slot].ring[dci][model_ep_dequeue[slot][dci] * TRB_WORDS];
                    if (((last[3] >> TRB_TYPE_SHIFT) & TRB_TYPE_MASK) == TRB_TYPE_LINK) {
                        model_ep_dequeue[slot][dci] = 0;
                        if (last[3] & TRB_TOGGLE) {
                            model_ep_cycle[slot][dci] ^= TRB_CYCLE;
                        }
                        last = &hc->slots[slot].ring[dci][0];
                    }
                    if (model_fetched_n < 64) {
                        model_fetched[model_fetched_n++] = last;
                    }
                }
                if (terminal_after_short && (last[3] & TRB_IOC)) {
                    post_transfer(TRB_COMPLETION_SHORT, last[2] & 0x1FFFFu, slot, dci, last);
                }
                model_td_pos = 0;
                model_ep_advance(slot, dci);
                continue;
            }
            model_move(slot, dci, t, bytes);
        }
        if (type == TRB_TYPE_STATUS && status_code != 0 && transfer_answers) {
            /* The status stage answered on its own: a stall there halts
               the endpoint after a data stage that answered well. */
            post_transfer((uint32_t)status_code, 0, slot, dci, t);
            if (status_code == TRB_COMPLETION_STALL) {
                model_ep_halted[slot][dci] = 1;
            }
            status_code = 0;
            model_ep_advance(slot, dci);
            if (model_ep_halted[slot][dci]) {
                break;
            }
            continue;
        }
        if ((t[3] & TRB_IOC) && transfer_answers) {
            post_transfer((uint32_t)transfer_code, transfer_residual, slot, dci, t);
        }
        model_ep_advance(slot, dci);
    }
}

/* The driver points the ring somewhere after an abort; the controller reads
   from where it is told. The address written is truncated on this host, so the
   model takes the driver's own position rather than decoding it. */
static void ring_moved(void)
{
    model_cmd_dequeue = hc->cmd_enqueue;
    model_cmd_cycle = hc->cmd_cycle;
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
   about it. Sixteen hc->slots, four ports, 64-byte contexts, no scratchpads, and
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
    delayed_us = 0;
    halt_after_us = 0;
    stop_halted = 0;
    stop_timeout = 0;
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
                model_ep_in[si][di] = 0;
                model_ep_halted[si][di] = 0;
                model_ep_error[si][di] = 0;
            }
        }
    }
    medium_at = 0;
    model_td_pos = 0;
    short_at = 0;
    short_left = 0;
    terminal_after_short = 1;
    fail_at = 0;
    fail_code = 0;
    fail_left = 0;
    stop_events = 0;
    command_answer_code = 0;
    model_fetched_n = 0;
    virt_map = 0;
    model_pages_n = 0;
    watch_publish = 0;
    watch_result = -1;
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
    model_vendor = 0x8086;
    {
        unsigned c;

        for (c = 0; c < 256; c++) {
            pci_cfg[c] = 0;
        }
        /* The notebook this was written for: two of nine USB2 pins on this
           controller, the rest on the companion, and one SuperSpeed pin
           enabled of four. */
        pci_cfg[0xD0] = 0x00A;
        pci_cfg[0xD4] = 0x1FF;
        pci_cfg[0xD8] = 0x1;
        pci_cfg[0xDC] = 0xF;
    }
    model_busy = 0;
    model_raises = 0;
    msi_vector = -1;
    msi_refuses = 0;
    model_irq = 7;
    intx_enabled = 0;
    event_on_iman = 0;
    model_bar0 = 0xFEBF0004u; /* a 64-bit memory aperture */
    model_bar1 = 0;
    xhci_set_io(&model_io);
}

/* A device on a root port, with no hub above it. */
static struct usb_location root_at(int port)
{
    struct usb_location where;

    where.root_port = port;
    where.depth = 0;
    where.route = 0;
    where.tt_device = 0;
    where.tt_port = 0;
    where.tt_multi = 0;
    return where;
}

/* ---- a device with a bulk pair, and the buffers the transfer rows use ---- */

/* The rows over a 32 KiB transfer: the buffer the driver is given, whose
   pages are reached through the map below, and the pages the controller
   actually moves data to and from, in the order the map chooses. */
static uint8_t big[BIG_PAGES * 4096] __attribute__((aligned(4096)));
static uint8_t shadow[BIG_PAGES * 4096] __attribute__((aligned(4096)));
static unsigned perm[BIG_PAGES];
static int map_fail_at; /* the page whose translation fails, from one; 0 for none */

/* The page of `shadow` that stands for a byte of `big`, by the map, as a
   host pointer; the driver's addresses are the kernel's 32 bits, so the
   map works on the low word of the address it is given. */
static uint8_t *shadow_of(uint32_t lo)
{
    uint32_t base = (uint32_t)(uintptr_t)big;
    unsigned page;

    if (lo - base >= sizeof(big)) {
        return 0;
    }
    page = (unsigned)((lo - base) / 4096);
    if (map_fail_at != 0 && (int)page + 1 == map_fail_at) {
        return 0;
    }
    return shadow + perm[page] * 4096 + (lo & 0xFFFu);
}

static uintptr_t permuted_map(uintptr_t va)
{
    uint8_t *p = shadow_of((uint32_t)va);

    if ((uint32_t)va - (uint32_t)(uintptr_t)big >= sizeof(big)) {
        return va;
    }
    return (uintptr_t)p;
}

/* The controller's address of a byte of `big`, as the map gives it. */
static uint8_t *at_phys(const uint8_t *va)
{
    return shadow_of((uint32_t)(uintptr_t)va);
}

static void bulk_pair_up(void)
{
    struct usb_command_result result;
    struct usb_endpoint ep;
    struct usb_location where = root_at(0);
    unsigned i;

    model_reset();
    xhci_read_caps(hc);
    xhci_halt_reset(hc);
    xhci_start(hc);
    reg[M_PORTSC(0)] |= M_PORTSC_CCS | (3u << 10);
    xhci_device_open(hc, &where, USB_SPEED_HIGH);
    xhci_events(hc);
    xhci_command_done(hc, &result);
    xhci_device_address(hc, 1);
    xhci_events(hc);
    xhci_command_done(hc, &result);
    ep.number = 1;
    ep.direction = USB_DIR_IN;
    ep.type = USB_EP_BULK;
    ep.max_packet = 512;
    ep.interval = 0;
    xhci_endpoint_open(hc, 1, &ep);
    xhci_events(hc);
    xhci_command_done(hc, &result);
    ep.number = 2;
    ep.direction = USB_DIR_OUT;
    xhci_endpoint_open(hc, 1, &ep);
    xhci_events(hc);
    xhci_command_done(hc, &result);
    model_ep_in[1][3] = 1;
    model_ep_in[1][4] = 0;
    for (i = 0; i < BIG_PAGES; i++) {
        perm[i] = BIG_PAGES - 1 - i; /* the pages in the reverse order */
        model_page_register(shadow + i * 4096);
    }
    map_fail_at = 0;
    virt_map = permuted_map;
    for (i = 0; i < sizeof(medium); i++) {
        /* No two pages alike: the page's number enters each byte, so two
           pages moved to each other's place are a failure. */
        medium[i] = (uint8_t)((i * 7u + 3u) ^ (i >> 12));
    }
    for (i = 0; i < sizeof(shadow); i++) {
        shadow[i] = 0;
    }
}

/* The ring of `dci` as a driver that filled it up to `upto` in its first lap
   leaves it: those entries carry the first lap's cycle bit, so that after the
   Link toggles the cycle they are not valid again. A row that starts a
   transfer near the ring's end sets this up first. */
static void ring_lap_one(uint32_t dci, uint32_t upto)
{
    uint32_t i;

    for (i = 0; i < upto; i++) {
        hc->slots[1].ring[dci][i * TRB_WORDS + 3] = TRB_CYCLE;
    }
    hc->slots[1].enqueue[dci] = upto;
    model_ep_dequeue[1][dci] = upto;
}

/* The block at ring index `at` of endpoint `dci` of device one. */
static const uint32_t *block(uint32_t dci, uint32_t at)
{
    return &hc->slots[1].ring[dci][at * TRB_WORDS];
}

/* Whether the bytes the controller moved for the transfer at `buffer`,
   `length` long, match the medium from `from`, page by page where the map
   put them. */
static int landed(const uint8_t *buffer, uint32_t length, uint32_t from)
{
    uint32_t i;

    for (i = 0; i < length; i++) {
        if (*at_phys(buffer + i) != medium[from + i]) {
            return 0;
        }
    }
    return 1;
}

/* Whether the medium holds, from `from`, the bytes of the transfer at
   `buffer` as the map placed them. */
static int arrived(const uint8_t *buffer, uint32_t length, uint32_t from)
{
    uint32_t i;

    for (i = 0; i < length; i++) {
        if (medium[from + i] != *at_phys(buffer + i)) {
            return 0;
        }
    }
    return 1;
}

/** Check descending endpoint indices in both device context sizes. */
static void endpoint_order_rows(void)
{
    struct usb_location where = root_at(0);
    struct usb_command_result result;
    struct usb_endpoint ep;
    unsigned context_size;
    unsigned n;
    const int numbers[] = {1, 1, 15, 15};
    const int directions[] = {USB_DIR_IN, USB_DIR_OUT, USB_DIR_IN, USB_DIR_OUT};
    const uint32_t highest[] = {3, 3, 31, 31};

    for (context_size = 32; context_size <= 64; context_size += 32) {
        model_reset();
        if (context_size == 32) {
            reg[R_HCCPARAMS1] &= ~4u;
        }
        CHECK("endpoint order capabilities", xhci_read_caps(hc) == 0);
        CHECK("endpoint order reset", xhci_halt_reset(hc) == 0);
        CHECK("endpoint order start", xhci_start(hc) == 0);
        CHECK("endpoint order slot", xhci_device_open(hc, &where, USB_SPEED_HIGH) == 0);
        xhci_events(hc);
        CHECK("endpoint order enable completed", xhci_command_done(hc, &result) == 1);
        CHECK("endpoint order address", xhci_device_address(hc, 1) == 0);
        xhci_events(hc);
        CHECK("endpoint order address completed", xhci_command_done(hc, &result) == 1);
        for (n = 0; n < 4; n++) {
            ep.number = numbers[n];
            ep.direction = directions[n];
            ep.type = USB_EP_BULK;
            ep.max_packet = 512;
            ep.interval = 0;
            CHECK("endpoint order configure", xhci_endpoint_open(hc, 1, &ep) == 0);
            CHECK("context entries retains highest endpoint index",
                  (context_entry(hc, hc->input_ctx, 1)[0] >> 27) == highest[n]);
            xhci_events(hc);
            CHECK("endpoint order configure completed", xhci_command_done(hc, &result) == 1);
        }
    }
}

/** Check the shutdown result against the halted bit and the poll bound. */
static void stop_rows(void)
{
    int delayed;

    for (delayed = 0; delayed < 2; delayed++) {
        model_reset();
        CHECK("stop capabilities", xhci_read_caps(hc) == 0);
        CHECK("stop ownership", xhci_own(hc) == 0);
        CHECK("stop reset", xhci_halt_reset(hc) == 0);
        CHECK("stop start", xhci_start(hc) == 0);
        halts = 0;
        delayed_us = 0;
        halt_after_us = delayed ? 16000u : 0u;
        xhci_stop(hc);
        CHECK("shutdown polls for at most sixteen milliseconds", delayed_us == 16000u);
        CHECK("shutdown retains operating-system ownership", (reg[XECP] & LEGSUP_OS_OWNED) != 0);
        if (delayed) {
            CHECK("halt at the deadline reports success", stop_halted == 1 && stop_timeout == 0);
        } else {
            CHECK("halt timeout does not report success", stop_halted == 0 && stop_timeout == 1);
        }
    }
}

/** Check register ranges at the aperture boundary and near 32-bit wrap. */
static void register_range_rows(void)
{
    const uint32_t slots = 1u | (1u << 8) | (4u << 24);

    model_reset();
    reg[R_HCSPARAMS1] = slots;
    reg[R_DBOFF] = 0xFFFFFFFCu;
    CHECK("wrapping doorbell range is refused", xhci_read_caps(hc) != 0);
    model_reset();
    reg[R_HCSPARAMS1] = slots;
    reg[R_RTSOFF] = 0xFFFFFFE0u;
    CHECK("wrapping runtime range is refused", xhci_read_caps(hc) != 0);
    model_reset();
    reg[R_HCSPARAMS1] = slots;
    reg[R_DBOFF] = 0xFFF8u;
    reg[R_RTSOFF] = 0xFFC0u;
    CHECK("register ranges ending at aperture boundary fit", xhci_read_caps(hc) == 0);
    reg[R_DBOFF] = 0xFFFCu;
    CHECK("last doorbell must fit whole", xhci_read_caps(hc) != 0);
    reg[R_DBOFF] = 0xFFF8u;
    reg[R_RTSOFF] = 0xFFE0u;
    CHECK("first interrupter must fit whole", xhci_read_caps(hc) != 0);
    model_reset();
    reg[R_HCSPARAMS1] = 1u | (4u << 24);
    CHECK("controller without an interrupter is refused", xhci_read_caps(hc) != 0);
}

int main(void)
{
    struct usb_location root0 = root_at(0);
    const struct xhci_caps *c;
    struct usb_port_status status;
    struct usb_command_result result;
    unsigned i;

    /* Finding the controller. */
    model_reset();
    CHECK("a controller with a memory aperture is taken", discover() == 0);
    CHECK("found by the class the standard gives USB",
          searched_class == PCI_CLASS_SERIAL_BUS && searched_subclass == PCI_SUBCLASS_USB);
    CHECK("with its registers decoded", memory_space_enabled == 1);
    CHECK("and allowed to reach memory itself", bus_master_enabled == 1);

    model_reset();
    model_present = 0;
    CHECK("a bus with no xHCI is refused", discover() != 0);

    model_reset();
    model_progif = 0x20;
    CHECK("an EHCI is not mistaken for an xHCI", discover() != 0);

    model_reset();
    model_bar0 = 0xFEBF0001u;
    CHECK("an aperture in I/O space is refused", discover() != 0);

    model_reset();
    model_bar1 = 0x1u;
    CHECK("an aperture above 4 GiB is refused", discover() != 0);

    model_reset();
    model_bar0 = 0x4u;
    CHECK("an aperture that was never programmed is refused", discover() != 0);

    model_reset();
    pm_state = 3;
    CHECK("a controller parked in D3 is taken", discover() == 0);
    CHECK("and put back in D0, where its registers answer", pm_state == 0);
    CHECK("so its capability registers can be read at all", xhci_read_caps(hc) == 0);

    /* What the controller says about itself. */
    model_reset();
    CHECK("the capability registers are read", xhci_read_caps(hc) == 0);
    c = xhci_capabilities(hc);
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
    CHECK("a controller with a larger segment table is read", xhci_read_caps(hc) == 0);
    CHECK("and its limit is the field, not a guess", xhci_capabilities(hc)->erst_max == 4);

    model_reset();
    reg[R_HCCPARAMS1] = 0x1u | ((XECP / 4u) << 16); /* CSZ clear */
    CHECK("32-byte contexts are read from the same field", xhci_read_caps(hc) == 0);
    CHECK("and are 32 bytes", xhci_capabilities(hc)->context_size == 32);

    /* The scratchpad count is five bits in each half of one word, with the
       low five at the top: a driver that read either half alone would ask for
       the wrong number of pages. */
    model_reset();
    reg[R_HCSPARAMS2] = (1u << 21) | (3u << 27);
    CHECK("a split scratchpad count is read", xhci_read_caps(hc) == 0);
    CHECK("and is both halves", xhci_capabilities(hc)->scratchpads == 35u);

    model_reset();
    reg[OP(O_PAGESIZE)] = 0x2u; /* 8 KiB only */
    CHECK("a controller that does not take 4 KiB pages is refused", xhci_read_caps(hc) != 0);

    model_reset();
    reg[R_HCSPARAMS1] = 16u | (1u << 8) | (255u << 24);
    reg[R_DBOFF] = 0x20000u;
    CHECK("registers outside the mapped aperture are refused", xhci_read_caps(hc) != 0);

    model_reset();
    reg[R_CAPLENGTH] = 0x10u;
    CHECK("a capability register set too short to be one is refused", xhci_read_caps(hc) != 0);

    /* What must fit is the end of each register set, not its first byte. */
    model_reset();
    reg[R_DBOFF] = 0xFFF0u;
    reg[R_HCSPARAMS1] = 64u | (1u << 8) | (4u << 24);
    CHECK("a doorbell array that runs off the end is refused", xhci_read_caps(hc) != 0);

    model_reset();
    reg[R_RTSOFF] = 0xFFE0u;
    reg[R_HCSPARAMS1] = 16u | (8u << 8) | (4u << 24);
    CHECK("interrupters that run off the end are refused", xhci_read_caps(hc) != 0);

    register_range_rows();
    endpoint_order_rows();
    stop_rows();

    /* Taking the controller from the firmware. */
    model_reset();
    reg[XECP] = 1u | LEGSUP_BIOS_OWNED;
    reg[LEGCTL] = 0xE000FFFFu;
    CHECK("the capability list is read before it is walked", xhci_read_caps(hc) == 0);
    CHECK("firmware that answers hands the controller over", xhci_own(hc) == 0);
    CHECK("and its ownership bit is clear", (reg[XECP] & LEGSUP_BIOS_OWNED) == 0);
    CHECK("and this kernel's is set", (reg[XECP] & LEGSUP_OS_OWNED) != 0);
    CHECK("the firmware's interrupts are turned off", (reg[LEGCTL] & 0x0000E1FEu) == 0);
    CHECK("and what they had already raised is cleared", (reg[LEGCTL] & 0xE0000000u) == 0);

    model_reset();
    reg[XECP] = 1u | LEGSUP_BIOS_OWNED;
    handoff_delay = 200;
    CHECK("the capability list is read", xhci_read_caps(hc) == 0);
    CHECK("firmware that answers slowly is waited for", xhci_own(hc) == 0);
    CHECK("and hands the controller over in the end", (reg[XECP] & LEGSUP_BIOS_OWNED) == 0);

    model_reset();
    reg[XECP] = 1u | LEGSUP_BIOS_OWNED;
    handoff_answers = 0;
    CHECK("the capability list is read", xhci_read_caps(hc) == 0);
    CHECK("firmware that never answers is refused", xhci_own(hc) != 0);

    model_reset();
    reg[R_HCCPARAMS1] = 0x1u | 0x4u; /* no capability list at all */
    CHECK("a controller with no capability list is read", xhci_read_caps(hc) == 0);
    CHECK("and is already this kernel's", xhci_own(hc) == 0);

    /* What was taken from the firmware is given back, whether the driver is
       finished with it or could not use it at all. A machine whose firmware
       emulates a PS/2 keyboard out of a USB one would otherwise lose that
       keyboard to a kernel that had already given up. */
    model_reset();
    reg[XECP] = 1u | LEGSUP_BIOS_OWNED;
    reg[LEGCTL] = 0x0000E1FEu;
    CHECK("the capability list is read", xhci_read_caps(hc) == 0);
    CHECK("the controller is taken", xhci_own(hc) == 0);
    CHECK("its interrupts are off", (reg[LEGCTL] & 0x0000E1FEu) == 0);
    xhci_release(hc);
    CHECK("and given back, the firmware's claim restored", (reg[XECP] & LEGSUP_OS_OWNED) == 0);
    CHECK("with its own interrupts as they were", (reg[LEGCTL] & 0x0000E1FEu) == 0x0000E1FEu);
    xhci_release(hc);
    CHECK("giving back twice does nothing the second time", (reg[XECP] & LEGSUP_OS_OWNED) == 0);

    /* A controller that was started is halted on the way out, and its claim
       is not given back: restoring the firmware's interrupt sources wakes its
       system-management handler, which on a real machine took twenty seconds
       in the middle of a reboot. A bring-up that failed is the other case, and
       it does give the controller back, which the rows above check. */
    model_reset();
    reg[XECP] = 1u | LEGSUP_BIOS_OWNED;
    reg[LEGCTL] = 0x0000E1FEu;
    CHECK("the capability registers are read", xhci_read_caps(hc) == 0);
    CHECK("the controller is taken", xhci_own(hc) == 0);
    CHECK("halted and reset", xhci_halt_reset(hc) == 0);
    CHECK("started", xhci_start(hc) == 0);
    xhci_stop(hc);
    CHECK("observed halt reports success", stop_halted == 1 && stop_timeout == 0);
    CHECK("a controller that was started is halted", (reg[OP(O_USBSTS)] & M_USBSTS_HCH) != 0);
    CHECK("and its claim is kept rather than handed back", (reg[XECP] & LEGSUP_OS_OWNED) != 0);
    CHECK("with the firmware's interrupts still off", (reg[LEGCTL] & 0x0000E1FEu) == 0);

    /* One that was never started is never written to on the way out. */
    model_reset();
    CHECK("the capability registers are read", xhci_read_caps(hc) == 0);
    reg[OP(O_USBCMD)] = 0xABCDu;
    xhci_stop(hc);
    CHECK("a controller that never started is not halted", reg[OP(O_USBCMD)] == 0xABCDu);

    model_reset();
    reg[XECP] = 1u | LEGSUP_BIOS_OWNED;
    accepts_os_claim = 0;
    CHECK("the capability list is read", xhci_read_caps(hc) == 0);
    CHECK("a controller that will not take this kernel's claim is refused", xhci_own(hc) != 0);

    /* The list belongs to the controller, so a driver may not trust it. A
       head outside the mapped window is the one malformed value a single
       entry can produce: a next pointer is a byte, so no one hop can leave a
       64 KiB aperture, and the case a walk has to survive is the circle
       below. */
    model_reset();
    reg[R_HCCPARAMS1] = 0x1u | 0x4u | (0x4000u << 16); /* a head at 64 KiB */
    CHECK("the capability list is read", xhci_read_caps(hc) == 0);
    CHECK("a list whose head is outside the aperture is refused", xhci_own(hc) != 0);

    model_reset();
    reg[XECP] = 3u; /* one entry, not the legacy one, ending the list */
    CHECK("the capability list is read", xhci_read_caps(hc) == 0);
    CHECK("a list with no legacy capability leaves the controller ours", xhci_own(hc) == 0);

    /* And a list whose first entry is inside the window but whose next step
       is not: the check has to be made on every step, not only on the head. */
    model_reset();
    reg[R_HCCPARAMS1] = 0x1u | 0x4u | ((0xFFF8u / 4u) << 16);
    reg[0xFFF8] = 3u | (0x2u << 8);
    CHECK("the capability list is read", xhci_read_caps(hc) == 0);
    CHECK("a list whose next step leaves the aperture is refused", xhci_own(hc) != 0);

    model_reset();
    reg[XECP] = 2u | (2u << 8); /* a supported-protocol capability first */
    reg[XECP + 8] = 1u;         /* then the legacy one */
    reg[LEGCTL + 8] = 0u;
    CHECK("a list is walked to its end", xhci_read_caps(hc) == 0);
    CHECK("and the legacy capability found past another", xhci_own(hc) == 0);
    CHECK("with ownership taken there", (reg[XECP + 8] & LEGSUP_OS_OWNED) != 0);

    /* Halting and resetting. */
    model_reset();
    CHECK("the capability registers are read first", xhci_read_caps(hc) == 0);
    CHECK("a controller that halts and resets is taken", xhci_halt_reset(hc) == 0);
    CHECK("the reset pause is taken before it is read again", read_too_early == 0);
    CHECK("and it is left halted", (reg[OP(O_USBSTS)] & M_USBSTS_HCH) != 0);

    model_reset();
    halts = 0;
    CHECK("the capability registers are read", xhci_read_caps(hc) == 0);
    reg[OP(O_USBSTS)] &= ~M_USBSTS_HCH;
    CHECK("a controller that never halts is refused", xhci_halt_reset(hc) != 0);

    /* A controller that is not ready yet may have nothing but its own status
       written, so the driver waits before it touches the command register. */
    model_reset();
    reg[OP(O_USBSTS)] |= M_USBSTS_CNR;
    cnr_delay = 5;
    CHECK("the capability registers are read", xhci_read_caps(hc) == 0);
    CHECK("a controller still starting is waited for", xhci_halt_reset(hc) == 0);

    model_reset();
    reg[OP(O_USBSTS)] |= M_USBSTS_CNR;
    becomes_ready = 0;
    cnr_delay = 5;
    CHECK("the capability registers are read", xhci_read_caps(hc) == 0);
    CHECK("a controller that is never ready is refused rather than written to",
          xhci_halt_reset(hc) != 0);

    model_reset();
    reset_completes = 0;
    CHECK("the capability registers are read", xhci_read_caps(hc) == 0);
    CHECK("a reset that never completes is refused", xhci_halt_reset(hc) != 0);

    model_reset();
    cnr_clears = 0;
    CHECK("the capability registers are read", xhci_read_caps(hc) == 0);
    CHECK("a controller that never becomes ready is refused", xhci_halt_reset(hc) != 0);

    /* The scratchpad pages. */
    model_reset();
    CHECK("a controller that requires no scratchpads is served", xhci_read_caps(hc) == 0);
    CHECK("with nothing to do", xhci_scratchpads(hc) == 0);

    model_reset();
    reg[R_HCSPARAMS2] = (0u << 21) | (3u << 27);
    CHECK("a controller that requires three pages is read", xhci_read_caps(hc) == 0);
    CHECK("and served", xhci_scratchpads(hc) == 0);
    CHECK("with three pages", hc->scratchpad_array[2] != 0 && hc->scratchpad_array[3] == 0);
    for (i = 0; i < 3; i++) {
        CHECK("each page-aligned", (hc->scratchpad_array[i] & 0xFFFu) == 0);
    }

    model_reset();
    reg[R_HCSPARAMS2] = (1u << 21) | (3u << 27); /* 35 pages */
    CHECK("a controller that requires thirty-five pages is read", xhci_read_caps(hc) == 0);
    CHECK("and served, since the pages come from the heap", xhci_scratchpads(hc) == 0);
    CHECK("with thirty-five of them",
          hc->scratchpad_array[34] != 0 && hc->scratchpad_array[35] == 0);

    model_reset();
    reg[R_HCSPARAMS2] = (31u << 21) | (31u << 27); /* 1023 pages */
    CHECK("a controller that requires more pages than the array holds is read",
          xhci_read_caps(hc) == 0);
    CHECK("and refused by name", xhci_scratchpads(hc) != 0);

    /* Starting it: the rings, the slots, and the ports powered. */
    model_reset();
    CHECK("the capability registers are read", xhci_read_caps(hc) == 0);
    CHECK("the controller is taken", xhci_own(hc) == 0);
    CHECK("halted and reset", xhci_halt_reset(hc) == 0);
    CHECK("given its scratchpads", xhci_scratchpads(hc) == 0);
    CHECK("a controller that starts is started", xhci_start(hc) == 0);
    CHECK("with every slot the controller offers, which is its own number",
          reg[OP(0x38u)] == hc->caps.max_slots);
    CHECK("its context array programmed", reg[OP(0x30u)] != 0);
    CHECK("its command ring programmed, running from the first block",
          (reg[OP(0x18u)] & ~0xFu) != 0 && (reg[OP(0x18u)] & 1u) != 0);
    CHECK("one segment in its event ring table", reg[RTSOFF + 0x20u + 0x08u] == 1);
    CHECK("the table naming the ring it built", hc->erst[0] == (uint32_t)(uintptr_t)hc->event_ring);
    CHECK("with the ring's length", hc->erst[2] == XHCI_RING_TRBS);
    CHECK("and nothing in the halves this kernel cannot address",
          hc->erst[1] == 0 && reg[RTSOFF + 0x20u + 0x14u] == 0 && reg[OP(0x34u)] == 0);
    CHECK("that table programmed", reg[RTSOFF + 0x20u + 0x10u] != 0);
    CHECK("and a dequeue pointer to start from", reg[RTSOFF + 0x20u + 0x18u] != 0);
    CHECK("it is running", (reg[OP(O_USBSTS)] & M_USBSTS_HCH) == 0);
    CHECK("and every port has power",
          (reg[M_PORTSC(0)] & M_PORTSC_PP) != 0 && (reg[M_PORTSC(3)] & M_PORTSC_PP) != 0);

    model_reset();
    starts = 0;
    CHECK("the capability registers are read", xhci_read_caps(hc) == 0);
    CHECK("halted and reset", xhci_halt_reset(hc) == 0);
    CHECK("a controller that will not start is refused", xhci_start(hc) != 0);

    /* Commands. */
    model_reset();
    CHECK("the capability registers are read", xhci_read_caps(hc) == 0);
    CHECK("halted and reset", xhci_halt_reset(hc) == 0);
    CHECK("started", xhci_start(hc) == 0);
    CHECK("a command is issued", xhci_command_issue(hc, TRB_TYPE_NOOP_COMMAND, 0, 0) == 0);
    CHECK("and reaches the controller as the command it was",
          commands_seen == 1 && last_command_type == TRB_TYPE_NOOP_COMMAND);
    CHECK("it is not finished until its event has been read", xhci_command_done(hc, &result) == 0);
    CHECK("and no second command may be issued meanwhile",
          xhci_command_issue(hc, TRB_TYPE_NOOP_COMMAND, 0, 0) != 0);
    xhci_events(hc);
    CHECK("once the event is read the command is finished", xhci_command_done(hc, &result) == 1);
    CHECK("and it succeeded", result.status == USB_XFER_OK);
    CHECK("so another may be issued", xhci_command_issue(hc, TRB_TYPE_NOOP_COMMAND, 0, 0) == 0);

    model_reset();
    command_completes = 0;
    CHECK("the capability registers are read", xhci_read_caps(hc) == 0);
    CHECK("halted and reset", xhci_halt_reset(hc) == 0);
    CHECK("started", xhci_start(hc) == 0);
    CHECK("a command is issued", xhci_command_issue(hc, TRB_TYPE_NOOP_COMMAND, 0, 0) == 0);
    CHECK("one that is never answered is still running", xhci_command_done(hc, &result) == 0);
    xhci_advance(hc, 2000);
    CHECK("until its deadline passes", xhci_command_done(hc, &result) == -1);
    /* The abort is a request, and the controller answers it by stopping the
       ring. Once it has, the driver may use the ring again. */
    command_completes = 1;
    CHECK("and once the controller has stopped the ring it is used again",
          xhci_command_issue(hc, TRB_TYPE_NOOP_COMMAND, 0, 0) == 0);
    xhci_events(hc);
    CHECK("with the next command answered", xhci_command_done(hc, &result) == 1);

    model_reset();
    command_completes = 0;
    abort_completes = 0;
    CHECK("the capability registers are read", xhci_read_caps(hc) == 0);
    CHECK("halted and reset", xhci_halt_reset(hc) == 0);
    CHECK("started", xhci_start(hc) == 0);
    CHECK("a command is issued", xhci_command_issue(hc, TRB_TYPE_NOOP_COMMAND, 0, 0) == 0);
    xhci_advance(hc, 2000);
    CHECK("a command that is never answered is given up on", xhci_command_done(hc, &result) == -1);
    CHECK("and while the controller still holds the ring nothing is issued on it",
          xhci_command_issue(hc, TRB_TYPE_NOOP_COMMAND, 0, 0) != 0);

    /* A command nobody waits for is aborted by the clock alone: the device
       that issued it was given up on at its own deadline, and the ring must
       not stay held for every device after it. */
    model_reset();
    command_completes = 0;
    CHECK("the capability registers are read", xhci_read_caps(hc) == 0);
    CHECK("halted and reset", xhci_halt_reset(hc) == 0);
    CHECK("started", xhci_start(hc) == 0);
    CHECK("a command is issued", xhci_command_issue(hc, TRB_TYPE_NOOP_COMMAND, 0, 0) == 0);
    CHECK("and the ring is not free while it runs", xhci_command_free(hc) == 0);
    xhci_advance(hc, 1100);
    command_completes = 1;
    CHECK("past its deadline the clock alone has it aborted and the ring free",
          xhci_command_free(hc) == 1);
    CHECK("and the next command is issued",
          xhci_command_issue(hc, TRB_TYPE_NOOP_COMMAND, 0, 0) == 0);

    /* A completion that names a block the driver is not waiting on belongs to
       a command already abandoned, and must not answer the one in flight. */
    model_reset();
    command_completes = 0;
    CHECK("the capability registers are read", xhci_read_caps(hc) == 0);
    CHECK("halted and reset", xhci_halt_reset(hc) == 0);
    CHECK("started", xhci_start(hc) == 0);
    CHECK("a command is issued", xhci_command_issue(hc, TRB_TYPE_NOOP_COMMAND, 0, 0) == 0);
    post_event(TRB_TYPE_COMMAND_COMPLETE, TRB_COMPLETION_SUCCESS, 0, 0xDEADBEEFu);
    xhci_events(hc);
    CHECK("a completion for another command does not finish this one",
          xhci_command_done(hc, &result) == 0);

    /* The command ring wraps through the link block at its end, and both
       sides change cycle there. */
    model_reset();
    CHECK("the capability registers are read", xhci_read_caps(hc) == 0);
    CHECK("halted and reset", xhci_halt_reset(hc) == 0);
    CHECK("started", xhci_start(hc) == 0);
    for (i = 0; i < XHCI_RING_TRBS - 1; i++) {
        xhci_command_issue(hc, TRB_TYPE_NOOP_COMMAND, 0, 0);
        xhci_events(hc);
        xhci_command_done(hc, &result);
    }
    CHECK("the ring's last block is a link back to its start",
          ((hc->cmd_ring[(XHCI_RING_TRBS - 1) * TRB_WORDS + 3] >> TRB_TYPE_SHIFT) &
           TRB_TYPE_MASK) == TRB_TYPE_LINK);
    CHECK("with the bit that changes the cycle there",
          (hc->cmd_ring[(XHCI_RING_TRBS - 1) * TRB_WORDS + 3] & TRB_TOGGLE) != 0);
    CHECK("and the driver's own cycle changed with it", hc->cmd_cycle == 0);
    CHECK("so the next command goes to the first block", hc->cmd_enqueue == 0);
    CHECK("a command after the wrap is issued",
          xhci_command_issue(hc, TRB_TYPE_NOOP_COMMAND, 0, 0) == 0);
    CHECK("with the new cycle", (hc->cmd_ring[3] & TRB_CYCLE) == 0);

    /* The event ring wraps too, and the driver has to follow the controller
       through it rather than stopping at the end. */
    model_reset();
    CHECK("the capability registers are read", xhci_read_caps(hc) == 0);
    CHECK("halted and reset", xhci_halt_reset(hc) == 0);
    CHECK("started", xhci_start(hc) == 0);
    for (i = 0; i < XHCI_RING_TRBS + 40; i++) {
        post_event(TRB_TYPE_PORT_CHANGE, TRB_COMPLETION_SUCCESS, 0, 1u << 24);
        if ((i % 64) == 0) {
            xhci_events(hc);
        }
    }
    xhci_events(hc);
    CHECK("every event is taken, wrap and all", hc->event_index == model_event_write);
    CHECK("and the driver's cycle follows the controller's", hc->event_cycle == model_event_cycle);
    CHECK("with the dequeue pointer written back", (reg[RTSOFF + 0x20u + 0x18u] & 0x8u) != 0);
    CHECK("and the port it named remembered", hc->port_changed[0] == 1);
    CHECK("with the dequeue pointer at the block it stopped on",
          (reg[RTSOFF + 0x20u + 0x18u] & ~0xFu) ==
              ((uint32_t)(uintptr_t)&hc->event_ring[hc->event_index * TRB_WORDS] & ~0xFu));

    /* Ports. */
    model_reset();
    CHECK("the capability registers are read", xhci_read_caps(hc) == 0);
    CHECK("halted and reset", xhci_halt_reset(hc) == 0);
    CHECK("started", xhci_start(hc) == 0);
    reg[M_PORTSC(0)] |= M_PORTSC_CCS | (3u << 10);
    CHECK("a port with a device says so", xhci_port_status(hc, 0, &status) == 0);
    CHECK("with the speed the controller reports", status.connected == 1 && status.speed == 3);
    CHECK("a port with nothing on it says that", xhci_port_status(hc, 1, &status) == 0);
    CHECK("and is not connected", status.connected == 0);
    CHECK("a port outside the controller's own count is refused",
          xhci_port_status(hc, 99, &status) != 0);

    CHECK("a port with a device is reset", xhci_port_reset_begin(hc, 0) == 0);
    CHECK("and the reset finished", xhci_port_reset_done(hc, 0) == 1);
    CHECK("with its change bit cleared afterwards", (reg[M_PORTSC(0)] & M_PORTSC_PRC) == 0);
    CHECK("a port with nothing on it is not reset", xhci_port_reset_begin(hc, 1) != 0);

    /* A contact bounces, so a connect is not acted on the moment it appears. */
    model_reset();
    CHECK("the capability registers are read", xhci_read_caps(hc) == 0);
    CHECK("halted and reset", xhci_halt_reset(hc) == 0);
    CHECK("started", xhci_start(hc) == 0);
    reg[M_PORTSC(0)] |= M_PORTSC_CCS;
    xhci_events(hc);
    CHECK("a connect is not settled the moment it appears", xhci_port_settled(hc, 0) == 0);
    xhci_advance(hc, 50);
    CHECK("nor half way through the interval", xhci_port_settled(hc, 0) == 0);
    xhci_advance(hc, 60);
    CHECK("and is settled once the interval has passed", xhci_port_settled(hc, 0) == 1);
    reg[M_PORTSC(0)] &= ~M_PORTSC_CCS;
    xhci_events(hc);
    CHECK("a port with nothing on it is never settled", xhci_port_settled(hc, 0) == 0);

    /* A controller with more ports than the machines here have. */
    model_reset();
    reg[R_HCSPARAMS1] = 16u | (1u << 8) | (40u << 24);
    CHECK("a controller with forty ports is read", xhci_read_caps(hc) == 0);
    CHECK("and every one of them is reached", xhci_capabilities(hc)->max_ports == 40);

    model_reset();
    port_reset_completes = 0;
    CHECK("the capability registers are read", xhci_read_caps(hc) == 0);
    CHECK("halted and reset", xhci_halt_reset(hc) == 0);
    CHECK("started", xhci_start(hc) == 0);
    reg[M_PORTSC(0)] |= M_PORTSC_CCS;
    CHECK("a reset is begun", xhci_port_reset_begin(hc, 0) == 0);
    CHECK("and is not finished", xhci_port_reset_done(hc, 0) == 0);
    xhci_advance(hc, 1000);
    CHECK("a reset that never finishes is given up on", xhci_port_reset_done(hc, 0) == -1);

    /* The write discipline this register needs, which is the easiest thing in
       the driver to get wrong: half its bits are cleared by writing them. */
    model_reset();
    CHECK("the capability registers are read", xhci_read_caps(hc) == 0);
    CHECK("halted and reset", xhci_halt_reset(hc) == 0);
    CHECK("started", xhci_start(hc) == 0);
    port_reset_completes = 0;
    reg[M_PORTSC(0)] |= M_PORTSC_CCS | M_PORTSC_PED | M_PORTSC_CSC;
    CHECK("a reset is begun", xhci_port_reset_begin(hc, 0) == 0);
    CHECK("without throwing away a connect nobody has handled yet",
          (reg[M_PORTSC(0)] & M_PORTSC_CSC) != 0);
    CHECK("and without disabling the port it is resetting", (reg[M_PORTSC(0)] & M_PORTSC_PED) != 0);

    /* Devices, endpoints and the recovery a halted one needs. */
    model_reset();
    CHECK("the capability registers are read", xhci_read_caps(hc) == 0);
    CHECK("halted and reset", xhci_halt_reset(hc) == 0);
    CHECK("started", xhci_start(hc) == 0);
    reg[M_PORTSC(0)] |= M_PORTSC_CCS | (3u << 10);
    {
        struct usb_location where;

        where.root_port = 0;
        where.depth = 0;
        where.route = 0;
        where.tt_device = 0;
        where.tt_port = 0;
        where.tt_multi = 0;
        CHECK("a slot is asked for", xhci_device_open(hc, &where, USB_SPEED_HIGH) == 0);
    }
    CHECK("with the command that asks", last_command_type == TRB_TYPE_ENABLE_SLOT);
    xhci_events(hc);
    CHECK("and the controller answers with one", xhci_command_done(hc, &result) == 1);
    CHECK("which is the device handle", result.device == 1);
    CHECK("the device is addressed", xhci_device_address(hc, 1) == 0);
    CHECK("with the command that addresses it", last_command_type == TRB_TYPE_ADDRESS_DEVICE);
    CHECK("naming the slot", last_command_slot == 1);
    CHECK("its context is where the controller will look", hc->dcbaa[1] != 0);
    xhci_events(hc);
    CHECK("the controller answers", xhci_command_done(hc, &result) == 1);

    /* Where a device is: the route string, the root port, the hub's own
       words, and the translator's for a device below a high-speed hub. The
       words are what the specification says; whether a controller schedules
       from them is the notebook's proof. */
    {
        struct usb_location where;
        const uint32_t *slot = (const uint32_t *)(const void *)&hc->input_ctx[64];
        int hub_slot;
        int hs_hub;

        where.root_port = 2;
        where.depth = 1;
        where.route = 0x3;
        where.tt_device = 0;
        where.tt_port = 0;
        where.tt_multi = 0;
        reg[M_PORTSC(2)] |= M_PORTSC_CCS | (3u << 10);
        CHECK("a device on port 3 of a hub on root port 2 is asked for",
              xhci_device_open(hc, &where, USB_SPEED_HIGH) == 0);
        xhci_events(hc);
        CHECK("and given a slot", xhci_command_done(hc, &result) == 1 && result.device == 2);
        CHECK("and addressed", xhci_device_address(hc, 2) == 0);
        CHECK("with the route string 0x3", (slot[0] & 0xFFFFFu) == 0x3u);
        CHECK("and the root port 2 in the second word", ((slot[1] >> 16) & 0xFFu) == 3u);
        CHECK("and no hub, translator or multi-TT bits",
              (slot[0] & ((1u << 25) | (1u << 26))) == 0 && slot[2] == 0);
        xhci_events(hc);
        xhci_command_done(hc, &result);

        where.root_port = 0;
        where.depth = 2;
        where.route = 0x23;
        CHECK("a device two tiers down is asked for",
              xhci_device_open(hc, &where, USB_SPEED_FULL) == 0);
        xhci_events(hc);
        xhci_command_done(hc, &result);
        CHECK("and addressed with both nibbles",
              xhci_device_address(hc, result.device) == 0 && (slot[0] & 0xFFFFFu) == 0x23u);
        xhci_events(hc);
        xhci_command_done(hc, &result);

        where.depth = 5;
        where.route = 0xF3333u;
        CHECK("a device on the fifth tier is accepted",
              xhci_device_open(hc, &where, USB_SPEED_FULL) == 0);
        xhci_events(hc);
        xhci_command_done(hc, &result);
        where.depth = 6;
        where.route = 0x1F3333u;
        CHECK("and one on a sixth tier is refused",
              xhci_device_open(hc, &where, USB_SPEED_FULL) != 0);
        where.depth = 1;
        where.route = 0x13;
        CHECK("as is a route with bits above its depth",
              xhci_device_open(hc, &where, USB_SPEED_FULL) != 0);
        where.depth = 1;
        where.route = 0xF;
        CHECK("port 15 of a hub fits the nibble", xhci_device_open(hc, &where, USB_SPEED_LOW) == 0);
        xhci_events(hc);
        xhci_command_done(hc, &result);
        CHECK("and is written as it is",
              xhci_device_address(hc, result.device) == 0 && (slot[0] & 0xFu) == 0xFu);
        xhci_events(hc);
        xhci_command_done(hc, &result);

        /* A hub: its own slot context gains the hub bit and its port count,
           through the evaluate command, and a high-speed hub with one
           translator per port gains the multi-TT bit and its think time. */
        where.depth = 0;
        where.route = 0;
        CHECK("a hub on a root port is asked for",
              xhci_device_open(hc, &where, USB_SPEED_HIGH) == 0);
        xhci_events(hc);
        xhci_command_done(hc, &result);
        hub_slot = result.device;
        CHECK("and addressed", xhci_device_address(hc, hub_slot) == 0);
        xhci_events(hc);
        xhci_command_done(hc, &result);
        CHECK("a full-speed hub is told its four ports",
              xhci_device_hub(hc, hub_slot, 4, 0, 0) == 0);
        CHECK("through the evaluate command", last_command_type == TRB_TYPE_EVALUATE_CTX);
        CHECK("adding the slot context alone",
              ((const uint32_t *)(const void *)&hc->input_ctx[0])[1] == 0x1u);
        CHECK("with the hub bit and the count",
              (slot[0] & (1u << 26)) != 0 && ((slot[1] >> 24) & 0xFFu) == 4u);
        CHECK("and nothing about translators", (slot[0] & (1u << 25)) == 0 && slot[2] == 0);
        xhci_events(hc);
        xhci_command_done(hc, &result);
        CHECK("a high-speed hub with a translator per port is told so",
              xhci_device_hub(hc, hub_slot, 7, 1, 2) == 0);
        CHECK("with the multi-TT bit and its think time in its own third word",
              (slot[0] & (1u << 25)) != 0 && ((slot[2] >> 16) & 3u) == 2u &&
                  ((slot[1] >> 24) & 0xFFu) == 7u);
        CHECK("and a think time above 3 refused", xhci_device_hub(hc, hub_slot, 7, 1, 4) != 0);
        xhci_events(hc);
        xhci_command_done(hc, &result);
        hs_hub = hub_slot;

        /* A low-speed device below that hub carries the hub's slot and port
           in its third word and the multi-TT bit, and nothing of the think
           time, which is the hub's alone. */
        where.depth = 1;
        where.route = 0x2;
        where.tt_device = hs_hub;
        where.tt_port = 2;
        where.tt_multi = 1;
        CHECK("a low-speed device below a high-speed hub is asked for",
              xhci_device_open(hc, &where, USB_SPEED_LOW) == 0);
        xhci_events(hc);
        xhci_command_done(hc, &result);
        CHECK("and addressed with the translator's slot and port",
              xhci_device_address(hc, result.device) == 0 &&
                  (slot[2] & 0xFFu) == (uint32_t)hs_hub && ((slot[2] >> 8) & 0xFFu) == 2u &&
                  ((slot[2] >> 16) & 3u) == 0u);
        CHECK("and the multi-TT bit of the hub it is under", (slot[0] & (1u << 25)) != 0);
        CHECK("and no hub bit of its own", (slot[0] & (1u << 26)) == 0);
        xhci_events(hc);
        xhci_command_done(hc, &result);

        /* A full-speed device below a full-speed hub below that high-speed
           hub uses the high-speed hub as its translator: what the location
           names is what is written, two tiers down. */
        where.depth = 2;
        where.route = 0x12; /* port 2 of a full-speed hub on port 1 of the high-speed one */
        where.tt_device = hs_hub;
        where.tt_port = 1; /* the high-speed hub's port, not the device's own */
        where.tt_multi = 1;
        CHECK("a device below a full-speed hub below the high-speed one is asked for",
              xhci_device_open(hc, &where, USB_SPEED_FULL) == 0);
        xhci_events(hc);
        xhci_command_done(hc, &result);
        CHECK("and carries the high-speed hub as its translator, at the hub's port",
              xhci_device_address(hc, result.device) == 0 &&
                  (slot[2] & 0xFFu) == (uint32_t)hs_hub && ((slot[2] >> 8) & 0xFFu) == 1u &&
                  (slot[0] & 0xFFFFFu) == 0x12u);
        xhci_events(hc);
        xhci_command_done(hc, &result);
    }

    /* Devices, endpoints and the recovery a halted one needs, again on a
       fresh controller. */
    model_reset();
    CHECK("the capability registers are read", xhci_read_caps(hc) == 0);
    CHECK("halted and reset", xhci_halt_reset(hc) == 0);
    CHECK("started", xhci_start(hc) == 0);
    reg[M_PORTSC(0)] |= M_PORTSC_CCS | (3u << 10);
    {
        struct usb_location where;

        where.root_port = 0;
        where.depth = 0;
        where.route = 0;
        where.tt_device = 0;
        where.tt_port = 0;
        where.tt_multi = 0;
        CHECK("a slot is asked for", xhci_device_open(hc, &where, USB_SPEED_HIGH) == 0);
        xhci_events(hc);
        xhci_command_done(hc, &result);
        CHECK("the device is addressed", xhci_device_address(hc, 1) == 0);
        xhci_events(hc);
        xhci_command_done(hc, &result);
    }

    {
        struct usb_endpoint ep;

        ep.number = 1;
        ep.direction = USB_DIR_IN;
        ep.type = USB_EP_INTERRUPT;
        ep.max_packet = 8;
        ep.interval = 10;
        CHECK("an endpoint is opened", xhci_endpoint_open(hc, 1, &ep) == 0);
        CHECK("with the command that configures one", last_command_type == TRB_TYPE_CONFIG_EP);
        xhci_events(hc);
        CHECK("the controller answers", xhci_command_done(hc, &result) == 1);
    }

    /* A halted endpoint is reset and its ring pointed at the block the driver
       will write next. Returning an error and leaving the ring alone would
       leave the endpoint unusable for the rest of the boot. */
    model_ep_halted[1][3] = 1; /* the controller reported the halt */
    CHECK("a halted endpoint is reset", xhci_endpoint_reset(hc, 1, 3) == 0);
    CHECK("with the command that resets one", last_command_type == TRB_TYPE_RESET_EP);
    xhci_events(hc);
    CHECK("the controller answers", xhci_command_done(hc, &result) == 1);
    CHECK("and its ring is pointed at the driver's next block",
          xhci_endpoint_dequeue_here(hc, 1, 3) == 0);
    CHECK("with the command that points it", last_command_type == TRB_TYPE_SET_TR_DEQUEUE);
    xhci_events(hc);
    CHECK("the controller answers", xhci_command_done(hc, &result) == 1);

    CHECK("a full-speed device's control endpoint is corrected once it is known",
          xhci_device_packet_size(hc, 1, 64) == 0);
    CHECK("with the command that changes one field of a context",
          last_command_type == TRB_TYPE_EVALUATE_CTX);
    xhci_events(hc);
    CHECK("the controller answers", xhci_command_done(hc, &result) == 1);

    CHECK("a device the controller never gave out is refused", xhci_device_address(hc, 3) != 0);
    /* Slots are numbered from one, so the last one the controller may give
       out is its own count and not one less. A driver whose tables were a
       slot short would refuse the last slot it is entitled to hand out. */
    CHECK("the last slot the controller may give out is not refused for being last",
          xhci_device_address(hc, (int)hc->nslots) != 0 && hc->slots[hc->nslots].used == 0);
    hc->slots[hc->nslots].used = 1;
    hc->slots[hc->nslots].port = 0;
    hc->slots[hc->nslots].speed = USB_SPEED_HIGH;
    CHECK("and is addressed like any other", xhci_device_address(hc, (int)hc->nslots) == 0);
    xhci_events(hc);
    CHECK("the controller answers", xhci_command_done(hc, &result) == 1);
    CHECK("and so is an endpoint on one", xhci_endpoint_reset(hc, 3, 3) != 0);

    /* The pins the chipset will give. A firmware leaves some on the companion
       controller for an operating system that cannot drive this one; this
       kernel has no driver for the companion, so a pin left there is a socket
       nothing can reach. */
    model_reset();
    CHECK("a controller with pins on the companion is taken", discover() == 0);
    CHECK("and every pin the chipset allows is moved to it", pci_cfg[0xD0] == 0x1FF);
    CHECK("with the SuperSpeed pins as well", pci_cfg[0xD8] == 0xF);

    /* Only the pins the mask allows: one the chipset will not move is one the
       write has no effect on, and writing it anyway would put a pin in the
       record that was never routed. */
    model_reset();
    pci_cfg[0xD0] = 0x001;
    pci_cfg[0xD4] = 0x00F;
    CHECK("a controller with fewer movable pins is taken", discover() == 0);
    CHECK("and takes those and no more", pci_cfg[0xD0] == 0x00F);

    model_reset();
    pci_cfg[0xD0] = 0x1FF;
    CHECK("a controller that already has them all is taken", discover() == 0);
    CHECK("and is left alone", pci_cfg[0xD0] == 0x1FF);

    model_reset();
    model_vendor = 0x1B36;
    CHECK("another maker's controller is taken", discover() == 0);
    CHECK("and its configuration space is not written",
          pci_cfg[0xD0] == 0x00A && pci_cfg[0xD8] == 0x1);

    /* Transfers. The control transfer is three blocks and one event, and the
       event is asked for on the block that says how much arrived. */
    {
        struct usb_transfer transfer;
        struct usb_completion completion;
        static uint8_t setup[8];
        static uint8_t buffer[64];
        const uint32_t *ring;

        model_reset();
        CHECK("the capability registers are read", xhci_read_caps(hc) == 0);
        CHECK("halted and reset", xhci_halt_reset(hc) == 0);
        CHECK("started", xhci_start(hc) == 0);
        reg[M_PORTSC(0)] |= M_PORTSC_CCS | (3u << 10);
        CHECK("a slot is asked for", xhci_device_open(hc, &root0, USB_SPEED_HIGH) == 0);
        xhci_events(hc);
        CHECK("the controller gives one", xhci_command_done(hc, &result) == 1);
        CHECK("the device is addressed", xhci_device_address(hc, 1) == 0);
        xhci_events(hc);
        CHECK("the controller answers", xhci_command_done(hc, &result) == 1);

        setup[0] = 0x80;
        setup[1] = 6;
        transfer.setup = setup;
        transfer.buffer = buffer;
        transfer.length = 18;
        transfer.direction = USB_DIR_IN;
        CHECK("a control transfer is queued", xhci_submit(hc, 1, 1, &transfer) == 0);
        /* The ring is the one the slot was given when it was addressed. */
        ring = hc->slots[1].ring[1];
        CHECK("as a setup block", ((ring[3] >> TRB_TYPE_SHIFT) & TRB_TYPE_MASK) == TRB_TYPE_SETUP);
        CHECK("carrying the eight bytes itself", (ring[3] & TRB_IDT) != 0);
        CHECK("saying which way the data goes", (ring[3] & (3u << 16)) == TRB_TRT_IN);
        CHECK("then a data block",
              ((ring[TRB_WORDS + 3] >> TRB_TYPE_SHIFT) & TRB_TYPE_MASK) == TRB_TYPE_DATA);
        CHECK("which asks for the event, because it is the one that says how much came",
              (ring[TRB_WORDS + 3] & TRB_IOC) != 0);
        CHECK("and a status block the other way",
              ((ring[2 * TRB_WORDS + 3] >> TRB_TYPE_SHIFT) & TRB_TYPE_MASK) == TRB_TYPE_STATUS);
        xhci_events(hc);
        CHECK("the transfer is reported", xhci_poll(hc, &completion) == 1);
        CHECK("for the device and endpoint it was on",
              completion.device == 1 && completion.endpoint == 1);
        CHECK("as having moved everything", completion.status == USB_XFER_OK);
        CHECK("with the length it asked for", completion.length == 18);
        CHECK("and nothing missing", completion.residual == 0);
        CHECK("and there is nothing else waiting", xhci_poll(hc, &completion) == 0);

        /* A device that answers with less than was asked for. */
        transfer_code = TRB_COMPLETION_SHORT;
        transfer_residual = 6;
        CHECK("a second transfer is queued", xhci_submit(hc, 1, 1, &transfer) == 0);
        xhci_events(hc);
        CHECK("a short answer is reported", xhci_poll(hc, &completion) == 1);
        CHECK("as short", completion.status == USB_XFER_SHORT);
        CHECK("with what did not arrive", completion.residual == 6);
        CHECK("against what was asked for", completion.length == 18);

        /* A halted endpoint. */
        transfer_code = TRB_COMPLETION_STALL;
        transfer_residual = 0;
        CHECK("a third transfer is queued", xhci_submit(hc, 1, 1, &transfer) == 0);
        xhci_events(hc);
        CHECK("a stall is reported", xhci_poll(hc, &completion) == 1);
        CHECK("as a stall, which is the one a caller recovers from",
              completion.status == USB_XFER_STALLED);

        /* The endpoint is halted: nothing is queued on it until it is reset
           and moved on. */
        transfer_code = TRB_COMPLETION_SUCCESS;
        CHECK("nothing is queued on the halted endpoint", xhci_submit(hc, 1, 1, &transfer) != 0);
        model_ep_halted[1][1] = 1;
        CHECK("Reset Endpoint is issued", xhci_endpoint_reset(hc, 1, 1) == 0);
        xhci_events(hc);
        CHECK("and answered", xhci_command_done(hc, &result) == 1 && result.status == USB_XFER_OK);
        CHECK("Set TR Dequeue is issued", xhci_endpoint_dequeue_here(hc, 1, 1) == 0);
        xhci_events(hc);
        CHECK("and answered", xhci_command_done(hc, &result) == 1 && result.status == USB_XFER_OK);
        xhci_endpoint_retire_all(hc, 1, 1);

        /* A data stage that answers well under a status stage that stalls:
           the transfer failed, whatever the data stage said. */
        status_code = TRB_COMPLETION_STALL;
        CHECK("a transfer whose status stage stalls is queued",
              xhci_submit(hc, 1, 1, &transfer) == 0);
        xhci_events(hc);
        CHECK("it is reported once as a stall", xhci_poll(hc, &completion) == 1 &&
                                                    completion.status == USB_XFER_STALLED &&
                                                    xhci_poll(hc, &completion) == 0);
        CHECK("and the endpoint is halted", xhci_endpoint_state(hc, 1, 1) == XHCI_EP_HALTED);
        CHECK("Reset Endpoint is issued", xhci_endpoint_reset(hc, 1, 1) == 0);
        xhci_events(hc);
        CHECK("and answered", xhci_command_done(hc, &result) == 1 && result.status == USB_XFER_OK);
        CHECK("Set TR Dequeue is issued", xhci_endpoint_dequeue_here(hc, 1, 1) == 0);
        xhci_events(hc);
        CHECK("and answered", xhci_command_done(hc, &result) == 1 && result.status == USB_XFER_OK);
        xhci_endpoint_retire_all(hc, 1, 1);

        /* A short data stage under a stalled status stage: the failure is
           the outcome and the bytes are the data stage's. */
        transfer_code = TRB_COMPLETION_SHORT;
        transfer_residual = 6;
        status_code = TRB_COMPLETION_STALL;
        CHECK("a transfer short in its data stage and stalled in its status stage is queued",
              xhci_submit(hc, 1, 1, &transfer) == 0);
        xhci_events(hc);
        CHECK("it is reported as a stall with the data stage's residual",
              xhci_poll(hc, &completion) == 1 && completion.status == USB_XFER_STALLED &&
                  completion.residual == 6);
        transfer_code = TRB_COMPLETION_SUCCESS;
        transfer_residual = 0;
        model_ep_halted[1][1] = 1;
        CHECK("Reset Endpoint is issued", xhci_endpoint_reset(hc, 1, 1) == 0);
        xhci_events(hc);
        CHECK("and answered", xhci_command_done(hc, &result) == 1 && result.status == USB_XFER_OK);
        CHECK("Set TR Dequeue is issued", xhci_endpoint_dequeue_here(hc, 1, 1) == 0);
        xhci_events(hc);
        CHECK("and answered", xhci_command_done(hc, &result) == 1 && result.status == USB_XFER_OK);
        xhci_endpoint_retire_all(hc, 1, 1);

        /* A transfer the controller never answers. */
        transfer_answers = 0;
        CHECK("a fourth transfer is queued", xhci_submit(hc, 1, 1, &transfer) == 0);
        xhci_events(hc);
        CHECK("and nothing is reported for it", xhci_poll(hc, &completion) == 0);
        transfer_answers = 1;

        /* An interrupt endpoint, which carries one block and no setup. */
        {
            struct usb_endpoint ep;

            ep.number = 1;
            ep.direction = USB_DIR_IN;
            ep.type = USB_EP_INTERRUPT;
            ep.max_packet = 8;
            ep.interval = 10;
            CHECK("an endpoint is opened", xhci_endpoint_open(hc, 1, &ep) == 0);
            xhci_events(hc);
            CHECK("the controller answers", xhci_command_done(hc, &result) == 1);
        }
        transfer.setup = 0;
        transfer.length = 8;
        CHECK("a report is queued on it", xhci_submit(hc, 1, 3, &transfer) == 0);
        CHECK("as one ordinary block",
              ((hc->slots[1].ring[3][3] >> TRB_TYPE_SHIFT) & TRB_TYPE_MASK) == TRB_TYPE_NORMAL);
        xhci_events(hc);
        CHECK("and is reported", xhci_poll(hc, &completion) == 1);
        CHECK("on the endpoint it was queued on", completion.endpoint == 3);

        /* A device that has gone, answering for a transfer nobody waits for. */
        CHECK("a report is queued", xhci_submit(hc, 1, 3, &transfer) == 0);
        hc->slots[1].used = 0;
        xhci_events(hc);
        CHECK("a completion for a slot the controller no longer holds is dropped",
              xhci_poll(hc, &completion) == 0);
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
        ep.number = 15;
        ep.direction = USB_DIR_IN;
        CHECK("the last endpoint a device may have is index 31", dci_of(&ep) == 31);
        ep.number = 16;
        CHECK("an endpoint number no device may have is refused", dci_of(&ep) == 0);
        ep.number = 8;
        ep.direction = USB_DIR_IN;
        CHECK("an endpoint a stick might pick is reached", dci_of(&ep) == 17);
    }

    /* ---- bulk endpoints, the larger tables, and the generation ---- */

    /*
     * A stick's two endpoints are bulk, and a bulk endpoint's context type is
     * 2 outward and 6 inward, against the interrupt endpoint's 3 and 7. The
     * packet size is the descriptor's and not a number this driver knows: 64
     * at full speed, 512 at high speed, 1024 at SuperSpeed.
     */
    model_reset();
    CHECK("the capability registers are read", xhci_read_caps(hc) == 0);
    CHECK("halted and reset", xhci_halt_reset(hc) == 0);
    CHECK("started", xhci_start(hc) == 0);
    reg[M_PORTSC(0)] |= M_PORTSC_CCS | (3u << 10);
    CHECK("a slot is asked for", xhci_device_open(hc, &root0, USB_SPEED_HIGH) == 0);
    xhci_events(hc);
    CHECK("the controller answers with one", xhci_command_done(hc, &result) == 1);
    CHECK("the device is addressed", xhci_device_address(hc, 1) == 0);
    xhci_events(hc);
    CHECK("the controller answers", xhci_command_done(hc, &result) == 1);
    {
        struct usb_endpoint ep;
        uint32_t *entry;

        ep.number = 2;
        ep.direction = USB_DIR_OUT;
        ep.type = USB_EP_BULK;
        ep.max_packet = 512;
        ep.interval = 0;
        CHECK("a bulk out endpoint is opened", xhci_endpoint_open(hc, 1, &ep) == 0);
        entry = (uint32_t *)(void *)&hc->input_ctx[(dci_of(&ep) + 1u) * 64u];
        CHECK("with the context type a bulk out endpoint has", ((entry[1] >> 3) & 7u) == 2u);
        CHECK("and the packet size the descriptor gave", (entry[1] >> 16) == 512u);
        xhci_events(hc);
        CHECK("the controller answers", xhci_command_done(hc, &result) == 1);

        ep.direction = USB_DIR_IN;
        CHECK("a bulk in endpoint is opened", xhci_endpoint_open(hc, 1, &ep) == 0);
        entry = (uint32_t *)(void *)&hc->input_ctx[(dci_of(&ep) + 1u) * 64u];
        CHECK("with the context type a bulk in endpoint has", ((entry[1] >> 3) & 7u) == 6u);
        xhci_events(hc);
        CHECK("the controller answers", xhci_command_done(hc, &result) == 1);

        ep.number = 8;
        ep.max_packet = 64;
        CHECK("an endpoint numbered eight is opened, which a stick may pick",
              xhci_endpoint_open(hc, 1, &ep) == 0);
        entry = (uint32_t *)(void *)&hc->input_ctx[(dci_of(&ep) + 1u) * 64u];
        CHECK("at its own index", dci_of(&ep) == 17);
        CHECK("with the packet size a full-speed endpoint has", (entry[1] >> 16) == 64u);
        xhci_events(hc);
        CHECK("the controller answers", xhci_command_done(hc, &result) == 1);

        ep.number = 1;
        ep.direction = USB_DIR_IN;
        ep.type = USB_EP_INTERRUPT;
        ep.max_packet = 8;
        ep.interval = 10;
        CHECK("an interrupt endpoint beside them is opened", xhci_endpoint_open(hc, 1, &ep) == 0);
        entry = (uint32_t *)(void *)&hc->input_ctx[(dci_of(&ep) + 1u) * 64u];
        CHECK("with the context type an interrupt in endpoint has", ((entry[1] >> 3) & 7u) == 7u);
        xhci_events(hc);
        CHECK("the controller answers", xhci_command_done(hc, &result) == 1);
    }

    /* A transfer carries a number of the caller's, and the completion gives
       it back: a caller that gave up on one at its deadline tells the answer
       to that from the answer to what it is waiting for now. */
    {
        struct usb_transfer transfer;
        struct usb_completion completion;
        uint8_t buffer[8];

        transfer.setup = 0;
        transfer.buffer = buffer;
        transfer.length = sizeof(buffer);
        transfer.direction = USB_DIR_IN;
        transfer.generation = 0x51;
        CHECK("a transfer is queued with a generation", xhci_submit(hc, 1, 3, &transfer) == 0);
        xhci_events(hc);
        CHECK("the completion is reported", xhci_poll(hc, &completion) == 1);
        CHECK("and carries the generation the transfer had", completion.generation == 0x51);

        transfer.generation = 0x52;
        CHECK("a second transfer is queued", xhci_submit(hc, 1, 3, &transfer) == 0);
        xhci_events(hc);
        CHECK("its completion is reported", xhci_poll(hc, &completion) == 1);
        CHECK("with the second generation and not the first", completion.generation == 0x52);
    }

    /* More transfer events than the completion queue holds: the drain stops
       at the queue's end and the rest wait on the event ring for the next
       pass. A drain that dropped the ninth would leave the transfer it
       answered outstanding forever, which for a hub's change endpoint is a
       hub that never reports again. */
    {
        struct usb_transfer transfer;
        struct usb_completion completion;
        uint8_t buffer[8];
        int got = 0;

        transfer.setup = 0;
        transfer.buffer = buffer;
        transfer.length = sizeof(buffer);
        transfer.direction = USB_DIR_IN;
        transfer.generation = 0x60;
        /* The queue is filled by hand, as eight transfers answered at once
           would fill it, and one more transfer is answered. */
        hc->completion_head = hc->completion_tail + XHCI_COMPLETIONS;
        CHECK("a transfer is queued against a full queue", xhci_submit(hc, 1, 3, &transfer) == 0);
        CHECK("a drain over a full queue takes nothing", xhci_events(hc) == 0);
        while (xhci_poll(hc, &completion) == 1) {
            got++;
        }
        CHECK("and the eight are polled", got == 8);
        CHECK("the event is taken by the next drain", xhci_events(hc) == 1);
        CHECK("and polled then", xhci_poll(hc, &completion) == 1 && completion.generation == 0x60);
    }

    /* A completion for a device that has been closed is dropped: the slot may
       already belong to the next device, and advancing that device's transfer
       with this one's completion is the defect the drop exists for. */
    {
        struct usb_transfer transfer;
        struct usb_completion completion;
        uint8_t buffer[8];

        transfer.setup = 0;
        transfer.buffer = buffer;
        transfer.length = sizeof(buffer);
        transfer.direction = USB_DIR_IN;
        transfer.generation = 0x53;
        CHECK("a transfer is queued and answered", xhci_submit(hc, 1, 3, &transfer) == 0);
        /* The controller has written the event; the device goes before the
           driver reads it, which is what a stick pulled mid-transfer is. */
        xhci_device_closed(hc, 1);
        xhci_events(hc);
        CHECK("a completion for a device that has gone is dropped",
              xhci_poll(hc, &completion) == 0);
    }

    /* Every slot the controller offers, and one more. */
    model_reset();
    CHECK("the capability registers are read", xhci_read_caps(hc) == 0);
    CHECK("halted and reset", xhci_halt_reset(hc) == 0);
    CHECK("started", xhci_start(hc) == 0);
    {
        int slot;

        reg[M_PORTSC(0)] |= M_PORTSC_CCS | (3u << 10);
        for (slot = 1; slot <= (int)hc->nslots; slot++) {
            CHECK("a slot is asked for", xhci_device_open(hc, &root0, USB_SPEED_HIGH) == 0);
            xhci_events(hc);
            CHECK("and given", xhci_command_done(hc, &result) == 1 && result.device == slot);
            CHECK("and it can be addressed", xhci_device_address(hc, result.device) == 0);
            xhci_events(hc);
            CHECK("the controller answers", xhci_command_done(hc, &result) == 1);
        }
        /* One past the last. A controller that answers with a handle past
           the slots it said it had is not trusted: everything that takes a
           handle refuses it rather than writing past the tables. */
        CHECK("one more slot is asked for", xhci_device_open(hc, &root0, USB_SPEED_HIGH) == 0);
        xhci_events(hc);
        CHECK("and the controller answers with a handle past the tables",
              xhci_command_done(hc, &result) == 1 && result.device == (int)hc->nslots + 1);
        CHECK("which cannot be addressed", xhci_device_address(hc, result.device) != 0);
        {
            struct usb_endpoint ep;

            ep.number = 1;
            ep.direction = USB_DIR_IN;
            ep.type = USB_EP_BULK;
            ep.max_packet = 512;
            ep.interval = 0;
            CHECK("and takes no endpoint", xhci_endpoint_open(hc, result.device, &ep) != 0);
        }
    }

    /* ---- the interrupter ---- */

    /* Turned on, it moderates nothing, both enables are set, and the device's
       capability is programmed with the vector it was given. */
    model_reset();
    CHECK("the capability registers are read", xhci_read_caps(hc) == 0);
    CHECK("halted and reset", xhci_halt_reset(hc) == 0);
    CHECK("started", xhci_start(hc) == 0);
    reg[M_IMOD] = 0x0FA0u;   /* a firmware that left four milliseconds there */
    reg[M_IMAN] = M_IMAN_IP; /* and an interrupt of its own still pending */
    CHECK("the interrupter is turned on", xhci_interrupts_enable(hc, 48) == 0);
    CHECK("and says so", xhci_interrupts_on(hc) == 1);
    CHECK("the moderation interval a firmware left is written to zero", reg[M_IMOD] == 0);
    CHECK("and the pending bit is left where it was, not acknowledged here",
          (reg[M_IMAN] & M_IMAN_IP) != 0);
    CHECK("the interrupter's own enable is set", (reg[M_IMAN] & M_IMAN_IE) != 0);
    CHECK("and the controller's", (reg[OP(O_USBCMD)] & M_USBCMD_INTE) != 0);
    CHECK("the device is told which vector to send", msi_vector == 48);

    /* A device with no MSI capability leaves the controller as it was: a
       controller with its interrupter on and nothing to send is a controller
       whose events nobody hears. */
    model_reset();
    msi_refuses = 1;
    CHECK("the capability registers are read", xhci_read_caps(hc) == 0);
    CHECK("halted and reset", xhci_halt_reset(hc) == 0);
    CHECK("started", xhci_start(hc) == 0);
    CHECK("a device that cannot send messages is refused", xhci_interrupts_enable(hc, 48) == -1);
    CHECK("and says so", xhci_interrupts_on(hc) == 0);
    CHECK("with the interrupter's enable left clear", (reg[M_IMAN] & M_IMAN_IE) == 0);
    CHECK("and the controller's", (reg[OP(O_USBCMD)] & M_USBCMD_INTE) == 0);

    /* The line: the same device raises its interrupts on the PCI line the
       firmware routed for it, IRQ 7 on the notebook. The command register's
       interrupt disable bit is cleared, both enables are set, and an event
       asserts the line through the status register's event bit, which the
       acknowledgement clears together with the pending bit; a level line
       left with either set is taken forever. */
    CHECK("the line is named", xhci_line_irq(hc) == 7);
    CHECK("the controller is put on its line", xhci_interrupts_enable_line(hc) == 7);
    CHECK("and says so", xhci_interrupts_on(hc) == 1);
    CHECK("the interrupt disable bit is cleared", intx_enabled == 1);
    CHECK("the interrupter's enable is set", (reg[M_IMAN] & M_IMAN_IE) != 0);
    CHECK("and the controller's", (reg[OP(O_USBCMD)] & M_USBCMD_INTE) != 0);
    post_event(TRB_TYPE_PORT_CHANGE, TRB_COMPLETION_SUCCESS, 0, 1u << 24);
    CHECK("an event raises an interrupt on the line", model_raises == 1);
    CHECK("and sets the status register's event bit", (reg[OP(O_USBSTS)] & M_USBSTS_EINT) != 0);
    xhci_interrupt_ack(hc);
    CHECK("the acknowledgement clears the pending bit", (reg[M_IMAN] & M_IMAN_IP) == 0);
    CHECK("and the event bit", (reg[OP(O_USBSTS)] & M_USBSTS_EINT) == 0);
    CHECK("and leaves the enable set", (reg[M_IMAN] & M_IMAN_IE) != 0);
    CHECK("the drain takes the event", xhci_events(hc) == 1);
    reg[M_ERDP] = 0;
    CHECK("an empty drain on the line still writes the dequeue pointer with the busy bit",
          xhci_events(hc) == 0 && (reg[M_ERDP] & M_ERDP_EHB) != 0);
    xhci_interrupts_disable(hc);
    CHECK("stopped, the controller is off its line", xhci_interrupts_on(hc) == 0);
    CHECK("with the interrupter's enable clear", (reg[M_IMAN] & M_IMAN_IE) == 0);

    /* A device that names no line is left to the timer. */
    model_reset();
    msi_refuses = 1;
    model_irq = 0;
    CHECK("the controller is found again", discover() == 0);
    CHECK("the capability registers are read", xhci_read_caps(hc) == 0);
    CHECK("halted and reset", xhci_halt_reset(hc) == 0);
    CHECK("started", xhci_start(hc) == 0);
    CHECK("a device with no line names none", xhci_line_irq(hc) == -1);
    CHECK("and is not put on one", xhci_interrupts_enable_line(hc) == -1);
    CHECK("with the interrupter's enable left clear", (reg[M_IMAN] & M_IMAN_IE) == 0);

    /* The acknowledgement: the pending bit written as one, the enable beside
       it kept. A bare write of the pending bit clears the enable, and the
       controller is silent from then on. */
    model_reset();
    CHECK("the capability registers are read", xhci_read_caps(hc) == 0);
    CHECK("halted and reset", xhci_halt_reset(hc) == 0);
    CHECK("started", xhci_start(hc) == 0);
    CHECK("the interrupter is turned on", xhci_interrupts_enable(hc, 48) == 0);
    post_event(TRB_TYPE_PORT_CHANGE, TRB_COMPLETION_SUCCESS, 0, 1u << 24);
    CHECK("an event raises an interrupt", model_raises == 1);
    CHECK("which the controller records as pending", (reg[M_IMAN] & M_IMAN_IP) != 0);
    xhci_interrupt_ack(hc);
    CHECK("the acknowledgement clears the pending bit", (reg[M_IMAN] & M_IMAN_IP) == 0);
    CHECK("and leaves the enable beside it set", (reg[M_IMAN] & M_IMAN_IE) != 0);

    /* The drain says the driver is finished with what it was given, and only
       then does the controller raise another. This is the second event's row:
       a dequeue pointer written with the busy bit as zero leaves the
       controller believing the driver is still reading. */
    CHECK("the drain takes the event", xhci_events(hc) == 1);
    CHECK("and writes the dequeue pointer with the busy bit", (reg[M_ERDP] & M_ERDP_EHB) != 0);
    post_event(TRB_TYPE_PORT_CHANGE, TRB_COMPLETION_SUCCESS, 0, 1u << 24);
    CHECK("so the next event raises an interrupt of its own", model_raises == 2);
    CHECK("and it is taken", xhci_events(hc) == 1);

    /*
     * A drain that finds nothing still says the driver is finished. The
     * controller sets the busy bit when it raises an interrupt and raises no
     * other until the driver writes it back, and the timer's pump can take
     * the event before the handler's message is delivered, which leaves the
     * handler with an empty ring and the bit still set. The model is put
     * straight into that state: the busy bit set with nothing in the ring,
     * which no sequence of its own can produce, because the timer's drain
     * would have cleared the bit itself.
     */
    model_reset();
    CHECK("the capability registers are read", xhci_read_caps(hc) == 0);
    CHECK("halted and reset", xhci_halt_reset(hc) == 0);
    CHECK("started", xhci_start(hc) == 0);
    CHECK("the interrupter is turned on", xhci_interrupts_enable(hc, 48) == 0);
    model_busy = 1;
    model_raises = 0;
    CHECK("the handler's drain finds nothing", xhci_interrupt_take(hc) == 0);
    CHECK("and still says the driver is finished with what it was given", model_busy == 0);
    post_event(TRB_TYPE_PORT_CHANGE, TRB_COMPLETION_SUCCESS, 0, 1u << 24);
    CHECK("so the controller raises the next interrupt", model_raises == 1);

    /* An event written between the acknowledgement and the drain is taken by
       that same drain. The model writes it from inside the acknowledgement,
       which is the window a handler that drained first would leave. */
    model_reset();
    CHECK("the capability registers are read", xhci_read_caps(hc) == 0);
    CHECK("halted and reset", xhci_halt_reset(hc) == 0);
    CHECK("started", xhci_start(hc) == 0);
    CHECK("the interrupter is turned on", xhci_interrupts_enable(hc, 48) == 0);
    event_on_iman = 1;
    CHECK("an event written during the acknowledgement is taken by the drain after it",
          xhci_interrupt_take(hc) == 1);

    /* Turned off, the controller keeps running and says nothing. */
    model_reset();
    CHECK("the capability registers are read", xhci_read_caps(hc) == 0);
    CHECK("halted and reset", xhci_halt_reset(hc) == 0);
    CHECK("started", xhci_start(hc) == 0);
    CHECK("the interrupter is turned on", xhci_interrupts_enable(hc, 48) == 0);
    xhci_interrupts_disable(hc);
    CHECK("turned off, the interrupter's enable is clear", (reg[M_IMAN] & M_IMAN_IE) == 0);
    CHECK("and the controller's", (reg[OP(O_USBCMD)] & M_USBCMD_INTE) == 0);
    CHECK("and the device is told to send nothing", msi_vector == -1);
    CHECK("and it says so", xhci_interrupts_on(hc) == 0);
    model_raises = 0;
    post_event(TRB_TYPE_PORT_CHANGE, TRB_COMPLETION_SUCCESS, 0, 1u << 24);
    CHECK("an event after that raises nothing", model_raises == 0);
    CHECK("and waits for a drain that is not an interrupt's", xhci_events(hc) == 1);

    /* ---- a 32 KiB transfer as one command, its blocks and its bytes ---- */

    /* Nine blocks over nine pages when the buffer starts mid-page, each at
       the physical page the map gives, with the packets left after it, the
       chain on all but the last, the short-packet event on the intermediate
       IN blocks and the event on the last; and the bytes landing page by
       page where the blocks said. */
    {
        struct usb_transfer transfer;
        struct usb_completion completion;
        static const uint32_t sizes[9] = {31, 31, 31, 31, 28, 20, 12, 4, 0};
        static const uint32_t lens[9] = {2048, 4096, 4096, 4096, 4096, 4096, 4096, 4096, 2048};
        uint32_t first, i, off = 0;
        int ok_addr = 1, ok_len = 1, ok_size = 1, ok_chain = 1, ok_isp = 1, ok_ioc = 1;

        bulk_pair_up();
        transfer.setup = 0;
        transfer.buffer = big + 2048;
        transfer.length = 32768;
        transfer.direction = USB_DIR_IN;
        transfer.generation = 0x100;
        first = hc->slots[1].enqueue[3];
        transfer_answers = 1;
        CHECK("a 32 KiB IN transfer is queued", xhci_submit(hc, 1, 3, &transfer) == 0);
        for (i = 0; i < 9; i++) {
            const uint32_t *t = block(3, first + i);

            ok_addr = ok_addr && t[0] == (uint32_t)(uintptr_t)at_phys(big + 2048 + off);
            ok_len = ok_len && (t[2] & 0x1FFFFu) == lens[i];
            ok_size = ok_size && ((t[2] >> TRB_TD_SIZE_SHIFT) & 0x1Fu) == sizes[i];
            ok_chain = ok_chain && ((t[3] & TRB_CHAIN) != 0) == (i < 8);
            ok_isp = ok_isp && ((t[3] & TRB_ISP) != 0) == (i < 8);
            ok_ioc = ok_ioc && ((t[3] & TRB_IOC) != 0) == (i == 8);
            off += lens[i];
        }
        CHECK("as nine blocks at the pages the map gives", ok_addr);
        CHECK("with the pieces' lengths", ok_len);
        CHECK("and the packets left after each, at most 31, none after the last", ok_size);
        CHECK("chained on all but the last", ok_chain);
        CHECK("asking for a short packet's event on the intermediate blocks", ok_isp);
        CHECK("and the event on the last", ok_ioc);
        xhci_events(hc);
        CHECK("it is reported once",
              xhci_poll(hc, &completion) == 1 && xhci_poll(hc, &completion) == 0);
        CHECK("as whole", completion.status == USB_XFER_OK && completion.residual == 0 &&
                              completion.length == 32768 && completion.generation == 0x100);
        CHECK("and the bytes landed page by page where the blocks said",
              landed(big + 2048, 32768, 0));
        CHECK("and the transfer is retired", xhci_td_retired(hc, 1, 0x100) == 1);
    }

    /* The same OUT: no short-packet events asked for, and the medium holding
       the bytes from the pages in the map's order. */
    {
        struct usb_transfer transfer;
        struct usb_completion completion;
        uint32_t first, i;
        int ok_isp = 1;

        bulk_pair_up();
        for (i = 0; i < 32768; i++) {
            /* A pattern that differs from page to page, so two pages
               swapped in the map would show. */
            *at_phys(big + 2048 + i) = (uint8_t)(i * 13u + 5u + (i >> 12) * 37u);
        }
        transfer.setup = 0;
        transfer.buffer = big + 2048;
        transfer.length = 32768;
        transfer.direction = USB_DIR_OUT;
        transfer.generation = 0x101;
        first = hc->slots[1].enqueue[4];
        CHECK("a 32 KiB OUT transfer is queued", xhci_submit(hc, 1, 4, &transfer) == 0);
        for (i = 0; i < 9; i++) {
            ok_isp = ok_isp && (block(4, first + i)[3] & TRB_ISP) == 0;
        }
        CHECK("with no short-packet event asked for", ok_isp);
        xhci_events(hc);
        CHECK("it is reported whole",
              xhci_poll(hc, &completion) == 1 && completion.status == USB_XFER_OK);
        CHECK("and the medium holds the bytes from the pages in the map's order",
              arrived(big + 2048, 32768, 0));
    }

    /* ---- a 512 KiB transfer as one descriptor of 129 blocks, and one
       piece more than a record holds refused before publication ---- */
    {
        struct usb_transfer transfer;
        struct usb_completion completion;
        uint32_t first, i, chained = 0, ioc_at = 0, total = 0, placed = 0, at = 0;

        bulk_pair_up();
        transfer.setup = 0;
        transfer.buffer = big + 2048;
        transfer.length = 524288;
        transfer.direction = USB_DIR_IN;
        transfer.generation = 0x300;
        first = hc->slots[1].enqueue[3];
        transfer_answers = 1;
        CHECK("a 512 KiB IN transfer is queued", xhci_submit(hc, 1, 3, &transfer) == 0);
        for (i = 0; i < 129; i++) {
            const uint32_t *t = block(3, first + i);

            chained += (t[3] & TRB_CHAIN) != 0;
            if (t[3] & TRB_IOC) {
                ioc_at = i + 1;
            }
            total += t[2] & 0x1FFFFu;
            /* Each block names its piece's physical page, as the map placed
               it, and its length: 2048 bytes of the first page, whole pages
               between, and 2048 of the last. */
            if (t[0] == (uint32_t)permuted_map((uintptr_t)(big + 2048 + at)) && t[1] == 0 &&
                (t[2] & 0x1FFFFu) == (i == 0 || i == 128 ? 2048u : 4096u)) {
                placed++;
            }
            at += t[2] & 0x1FFFFu;
        }
        CHECK("as 129 blocks chained on all but the last, the event on the last, the bytes whole",
              chained == 128 && ioc_at == 129 && total == 524288);
        CHECK("each block at its piece's own page with its piece's length", placed == 129);
        xhci_events(hc);
        CHECK("it is reported once as whole",
              xhci_poll(hc, &completion) == 1 && completion.status == USB_XFER_OK &&
                  completion.residual == 0 && completion.length == 524288 &&
                  completion.generation == 0x300 && xhci_poll(hc, &completion) == 0);
        CHECK("and the bytes landed page by page where the blocks said",
              landed(big + 2048, 524288, 0));
        CHECK("and the transfer is retired", xhci_td_retired(hc, 1, 0x300) == 1);

        transfer.length = 130 * 4096; /* 131 pieces from mid-page */
        transfer.generation = 0x301;
        first = hc->slots[1].enqueue[3];
        CHECK("a transfer of one piece more than a record holds is refused",
              xhci_submit(hc, 1, 3, &transfer) != 0);
        CHECK("and nothing of it was published", hc->slots[1].enqueue[3] == first);
    }

    /* A transfer started three blocks before the ring's end: the Link it
       crosses carries the chain bit, the events land, and the bytes cross
       the Link and the boundary to the next transfer whole. */
    {
        struct usb_transfer transfer;
        struct usb_completion completion;

        bulk_pair_up();
        ring_lap_one(3, XHCI_EP_RING_TRBS - 4);
        transfer.setup = 0;
        transfer.buffer = big;
        transfer.length = 16384;
        transfer.direction = USB_DIR_IN;
        transfer.generation = 0x102;
        CHECK("a transfer is queued across the ring's end", xhci_submit(hc, 1, 3, &transfer) == 0);
        CHECK("the Link carries the chain bit",
              (block(3, XHCI_EP_RING_TRBS - 1)[3] & TRB_CHAIN) != 0);
        CHECK("and the block after it is the transfer's last", (block(3, 0)[3] & TRB_IOC) != 0);
        xhci_events(hc);
        CHECK("it is reported whole", xhci_poll(hc, &completion) == 1 &&
                                          completion.status == USB_XFER_OK &&
                                          completion.residual == 0);
        CHECK("and the bytes crossed the Link", landed(big, 16384, 0));
        transfer.buffer = big + 16384;
        transfer.generation = 0x103;
        CHECK("the next transfer is queued after it", xhci_submit(hc, 1, 3, &transfer) == 0);
        xhci_events(hc);
        CHECK("and reported", xhci_poll(hc, &completion) == 1 && completion.generation == 0x103);
        CHECK("with its bytes after the first's", landed(big + 16384, 16384, 16384));
    }

    /* A short packet on the third block, with the last block's event after
       it as the specification says: one completion, short by the bytes not
       moved, the transfer retired by the last block's event and not before;
       then the same with no such event, as QEMU does: the completion
       delivered, the transfer unretired until it is stopped and dequeued,
       and a transfer after that completing. */
    {
        struct usb_transfer transfer;
        struct usb_completion completion;
        struct usb_command_result result;

        bulk_pair_up();
        transfer.setup = 0;
        transfer.buffer = big + 2048;
        transfer.length = 32768;
        transfer.direction = USB_DIR_IN;
        transfer.generation = 0x104;
        short_at = 3;
        short_left = 1000;
        terminal_after_short = 1;
        CHECK("a transfer is queued", xhci_submit(hc, 1, 3, &transfer) == 0);
        CHECK("and held", xhci_td_hold(hc, 1, 0x104) == 0);
        xhci_events(hc);
        CHECK("the short is reported once",
              xhci_poll(hc, &completion) == 1 && xhci_poll(hc, &completion) == 0);
        CHECK("as short by the bytes not moved",
              completion.status == USB_XFER_SHORT &&
                  completion.residual == 32768 - (2048 + 4096 + 3096));
        CHECK("and the transfer is retired by the last block's event",
              xhci_td_retired(hc, 1, 0x104) == 1);
        xhci_td_release(hc, 1, 0x104);

        transfer.generation = 0x105;
        short_at = 3;
        short_left = 1000;
        terminal_after_short = 0;
        CHECK("a transfer is queued on a controller that posts no last event",
              xhci_submit(hc, 1, 3, &transfer) == 0);
        CHECK("and held", xhci_td_hold(hc, 1, 0x105) == 0);
        xhci_events(hc);
        CHECK("the short is reported",
              xhci_poll(hc, &completion) == 1 && completion.status == USB_XFER_SHORT);
        CHECK("and the transfer is not retired", xhci_td_retired(hc, 1, 0x105) == 0);
        CHECK("Stop Endpoint is issued", xhci_endpoint_stop(hc, 1, 3) == 0);
        xhci_events(hc);
        CHECK("and answered", xhci_command_done(hc, &result) == 1 && result.status == USB_XFER_OK);
        CHECK("still not retired", xhci_td_retired(hc, 1, 0x105) == 0);
        CHECK("Set TR Dequeue is issued at the enqueue pointer",
              xhci_endpoint_dequeue_here(hc, 1, 3) == 0);
        xhci_events(hc);
        CHECK("and answered", xhci_command_done(hc, &result) == 1 && result.status == USB_XFER_OK);
        xhci_endpoint_retire_all(hc, 1, 3);
        CHECK("and the transfer is retired by the two", xhci_td_retired(hc, 1, 0x105) == 1);
        xhci_td_release(hc, 1, 0x105);
        transfer.generation = 0x106;
        CHECK("a transfer after the stop is queued", xhci_submit(hc, 1, 3, &transfer) == 0);
        xhci_events(hc);
        CHECK("and completes whole", xhci_poll(hc, &completion) == 1 &&
                                         completion.status == USB_XFER_OK &&
                                         completion.generation == 0x106);
    }

    /* A failure on the second block, the controller halting there and
       posting nothing more: one failed completion with the bytes through
       the block before plus the failing block's own, the endpoint halted,
       the transfer owned until the reset and the dequeue are answered. */
    {
        struct usb_transfer transfer;
        struct usb_completion completion;
        struct usb_command_result result;

        bulk_pair_up();
        transfer.setup = 0;
        transfer.buffer = big + 2048;
        transfer.length = 32768;
        transfer.direction = USB_DIR_IN;
        transfer.generation = 0x107;
        fail_at = 2;
        fail_code = TRB_COMPLETION_TXN_ERROR;
        fail_left = 500;
        CHECK("a transfer is queued", xhci_submit(hc, 1, 3, &transfer) == 0);
        CHECK("and held", xhci_td_hold(hc, 1, 0x107) == 0);
        xhci_events(hc);
        CHECK("the failure is reported once",
              xhci_poll(hc, &completion) == 1 && xhci_poll(hc, &completion) == 0);
        CHECK("with the bytes through the block before and the failing block's own",
              completion.status == USB_XFER_FAILED && completion.residual == 32768 - (2048 + 3596));
        CHECK("the endpoint is halted", xhci_endpoint_state(hc, 1, 3) == 1);
        CHECK("and the transfer is owned", xhci_td_retired(hc, 1, 0x107) == 0);
        CHECK("Reset Endpoint is issued", xhci_endpoint_reset(hc, 1, 3) == 0);
        xhci_events(hc);
        CHECK("and answered", xhci_command_done(hc, &result) == 1 && result.status == USB_XFER_OK);
        CHECK("the transfer is owned between the reset and the dequeue",
              xhci_td_retired(hc, 1, 0x107) == 0);
        CHECK("Set TR Dequeue is issued", xhci_endpoint_dequeue_here(hc, 1, 3) == 0);
        xhci_events(hc);
        CHECK("and answered", xhci_command_done(hc, &result) == 1 && result.status == USB_XFER_OK);
        xhci_endpoint_retire_all(hc, 1, 3);
        CHECK("and retired after", xhci_td_retired(hc, 1, 0x107) == 1);
        xhci_td_release(hc, 1, 0x107);
        transfer.generation = 0x108;
        CHECK("a transfer after the recovery is queued", xhci_submit(hc, 1, 3, &transfer) == 0);
        xhci_events(hc);
        CHECK("and completes whole",
              xhci_poll(hc, &completion) == 1 && completion.status == USB_XFER_OK);
    }

    /* A translation that fails on the fifth page, and a third transfer with
       no record free: refused with the ring and its pointer untouched. */
    {
        struct usb_transfer transfer;
        uint32_t before[XHCI_EP_RING_TRBS * TRB_WORDS];
        uint32_t enqueue, i;
        int same = 1;

        bulk_pair_up();
        transfer.setup = 0;
        transfer.buffer = big + 2048;
        transfer.length = 32768;
        transfer.direction = USB_DIR_IN;
        transfer.generation = 0x109;
        map_fail_at = 5;
        enqueue = hc->slots[1].enqueue[3];
        for (i = 0; i < XHCI_EP_RING_TRBS * TRB_WORDS; i++) {
            before[i] = hc->slots[1].ring[3][i];
        }
        CHECK("a transfer whose fifth page has no frame is refused",
              xhci_submit(hc, 1, 3, &transfer) != 0);
        for (i = 0; i < XHCI_EP_RING_TRBS * TRB_WORDS; i++) {
            same = same && before[i] == hc->slots[1].ring[3][i];
        }
        CHECK("with the ring's words as they were", same);
        CHECK("and the pointer as it was", hc->slots[1].enqueue[3] == enqueue);
        map_fail_at = 0;
        transfer_answers = 0;
        transfer.generation = 0x10A;
        CHECK("a transfer is queued and not answered", xhci_submit(hc, 1, 3, &transfer) == 0);
        transfer.generation = 0x10B;
        CHECK("a second is queued beside it", xhci_submit(hc, 1, 3, &transfer) == 0);
        for (i = 0; i < XHCI_EP_RING_TRBS * TRB_WORDS; i++) {
            before[i] = hc->slots[1].ring[3][i];
        }
        enqueue = hc->slots[1].enqueue[3];
        transfer.generation = 0x10C;
        CHECK("a third, with no record free, is refused", xhci_submit(hc, 1, 3, &transfer) != 0);
        same = 1;
        for (i = 0; i < XHCI_EP_RING_TRBS * TRB_WORDS; i++) {
            same = same && before[i] == hc->slots[1].ring[3][i];
        }
        CHECK("with the ring's words and the pointer as they were",
              same && hc->slots[1].enqueue[3] == enqueue);
        transfer_answers = 1;
    }

    /* A stop after a transfer: the dequeue at the enqueue pointer with the
       producer's cycle, and the next transfer, across the wrap, fetched
       from there and no old block executed. */
    {
        struct usb_transfer transfer;
        struct usb_completion completion;
        struct usb_command_result result;
        unsigned n, i;
        int only_new = 1;

        bulk_pair_up();
        ring_lap_one(3, XHCI_EP_RING_TRBS - 6);
        transfer.setup = 0;
        transfer.buffer = big;
        transfer.length = 8192;
        transfer.direction = USB_DIR_IN;
        transfer.generation = 0x10D;
        transfer_answers = 0;
        CHECK("a transfer is queued and not answered", xhci_submit(hc, 1, 3, &transfer) == 0);
        CHECK("Stop Endpoint is issued", xhci_endpoint_stop(hc, 1, 3) == 0);
        xhci_events(hc);
        CHECK("and answered after a Stopped event",
              xhci_command_done(hc, &result) == 1 && stop_events == 1);
        CHECK("Set TR Dequeue is issued", xhci_endpoint_dequeue_here(hc, 1, 3) == 0);
        xhci_events(hc);
        CHECK("and answered", xhci_command_done(hc, &result) == 1);
        CHECK("with the controller's pointer at the driver's",
              model_ep_dequeue[1][3] == hc->slots[1].enqueue[3] &&
                  model_ep_cycle[1][3] == hc->slots[1].cycle[3]);
        xhci_endpoint_retire_all(hc, 1, 3);
        transfer_answers = 1;
        model_fetched_n = 0;
        transfer.buffer = big + 8192;
        transfer.length = 16384;
        transfer.generation = 0x10E;
        n = hc->slots[1].enqueue[3];
        CHECK("the next transfer is queued across the wrap", xhci_submit(hc, 1, 3, &transfer) == 0);
        xhci_events(hc);
        CHECK("and completes", xhci_poll(hc, &completion) == 1 && completion.generation == 0x10E);
        for (i = 0; i < model_fetched_n; i++) {
            uint32_t at = (uint32_t)(model_fetched[i] - hc->slots[1].ring[3]) / TRB_WORDS;
            uint32_t want = (n + i) % (XHCI_EP_RING_TRBS - 1);

            only_new = only_new && at == want;
        }
        CHECK("and the controller fetched only its blocks, from the pointer on",
              only_new && model_fetched_n == 4);
    }

    /* Stopped events retire nothing and deliver nothing. */
    {
        struct usb_transfer transfer;
        struct usb_completion completion;
        uint32_t first;

        bulk_pair_up();
        transfer.setup = 0;
        transfer.buffer = big;
        transfer.length = 8192;
        transfer.direction = USB_DIR_IN;
        transfer.generation = 0x10F;
        transfer_answers = 0;
        first = hc->slots[1].enqueue[3];
        CHECK("a transfer is queued and not answered", xhci_submit(hc, 1, 3, &transfer) == 0);
        CHECK("and held", xhci_td_hold(hc, 1, 0x10F) == 0);
        post_transfer(TRB_COMPLETION_STOPPED, 0, 1, 3, block(3, first));
        post_transfer(TRB_COMPLETION_STOPPED_SHORT, 100, 1, 3, block(3, first + 1));
        post_transfer(TRB_COMPLETION_STOPPED_LEN, 0, 1, 3, block(3, first + 1));
        xhci_events(hc);
        CHECK("Stopped, Stopped Short and Stopped Length Invalid deliver nothing",
              xhci_poll(hc, &completion) == 0);
        CHECK("and retire nothing", xhci_td_retired(hc, 1, 0x10F) == 0);
        CHECK("and the endpoint is stopped", xhci_endpoint_state(hc, 1, 3) == 2);
        transfer_answers = 1;
    }

    /* Two transfers outstanding, the event for the first: its generation,
       not the latest submission's; and an event for no transfer, dropped
       and counted. */
    {
        struct usb_transfer transfer;
        struct usb_completion completion;
        uint32_t first;

        bulk_pair_up();
        transfer.setup = 0;
        transfer.buffer = big;
        transfer.length = 4096;
        transfer.direction = USB_DIR_IN;
        transfer.generation = 0x110;
        transfer_answers = 0;
        first = hc->slots[1].enqueue[3];
        CHECK("transfer A is queued", xhci_submit(hc, 1, 3, &transfer) == 0);
        transfer.buffer = big + 4096;
        transfer.generation = 0x111;
        CHECK("transfer B is queued after it", xhci_submit(hc, 1, 3, &transfer) == 0);
        post_transfer(TRB_COMPLETION_SUCCESS, 0, 1, 3, block(3, first));
        xhci_events(hc);
        CHECK("the event for A's block reports A",
              xhci_poll(hc, &completion) == 1 && completion.generation == 0x110);
        post_transfer(TRB_COMPLETION_SUCCESS, 0, 1, 3, block(3, first + 10));
        xhci_events(hc);
        CHECK("an event for no transfer is dropped", xhci_poll(hc, &completion) == 0);
        CHECK("and counted", xhci_dropped_events(hc, 1) == 1);
        transfer_answers = 1;
    }

    /* An endpoint numbered past the first few: its records are there like
       any other's. */
    {
        struct usb_transfer transfer;
        struct usb_completion completion;
        struct usb_command_result result;
        struct usb_endpoint ep;

        bulk_pair_up();
        ep.number = 4;
        ep.direction = USB_DIR_IN;
        ep.type = USB_EP_BULK;
        ep.max_packet = 512;
        ep.interval = 0;
        CHECK("endpoint 4 IN is opened", xhci_endpoint_open(hc, 1, &ep) == 0);
        xhci_events(hc);
        CHECK("and the controller answers", xhci_command_done(hc, &result) == 1);
        model_ep_in[1][9] = 1;
        transfer.setup = 0;
        transfer.buffer = big + 2048;
        transfer.length = 4096;
        transfer.direction = USB_DIR_IN;
        transfer.generation = 0x120;
        CHECK("a transfer on it is queued", xhci_submit(hc, 1, 9, &transfer) == 0);
        xhci_events(hc);
        CHECK("and completes", xhci_poll(hc, &completion) == 1 &&
                                   completion.status == USB_XFER_OK && completion.endpoint == 9);
    }

    /* A block the controller could not run: the endpoint is in the Error
       state, which a Set TR Dequeue alone moves on; nothing is queued on it
       until then. */
    {
        struct usb_transfer transfer;
        struct usb_completion completion;
        struct usb_command_result result;

        bulk_pair_up();
        transfer.setup = 0;
        transfer.buffer = big + 2048;
        transfer.length = 32768;
        transfer.direction = USB_DIR_IN;
        transfer.generation = 0x121;
        fail_at = 2;
        fail_code = 5; /* TRB Error */
        fail_left = 4096;
        CHECK("a transfer is queued", xhci_submit(hc, 1, 3, &transfer) == 0);
        CHECK("and held", xhci_td_hold(hc, 1, 0x121) == 0);
        xhci_events(hc);
        CHECK("a block the controller could not run fails the transfer",
              xhci_poll(hc, &completion) == 1 && completion.status == USB_XFER_FAILED);
        CHECK("and puts the endpoint in the Error state",
              xhci_endpoint_state(hc, 1, 3) == XHCI_EP_ERROR);
        transfer.generation = 0x122;
        CHECK("on which nothing is queued", xhci_submit(hc, 1, 3, &transfer) != 0);
        CHECK("Set TR Dequeue is issued", xhci_endpoint_dequeue_here(hc, 1, 3) == 0);
        xhci_events(hc);
        CHECK("and answered with success",
              xhci_command_done(hc, &result) == 1 && result.status == USB_XFER_OK);
        xhci_endpoint_retire_all(hc, 1, 3);
        CHECK("which retires the transfer", xhci_td_retired(hc, 1, 0x121) == 1);
        xhci_td_release(hc, 1, 0x121);
        CHECK("and the next transfer is queued", xhci_submit(hc, 1, 3, &transfer) == 0);
        xhci_events(hc);
        CHECK("and completes", xhci_poll(hc, &completion) == 1 && completion.status == USB_XFER_OK);
    }

    /* A held record outlives the next submission on its endpoint: the data
       transfer's record is queryable at its wrapper's completion. */
    {
        struct usb_transfer transfer;
        struct usb_completion completion;

        bulk_pair_up();
        transfer.setup = 0;
        transfer.buffer = big + 2048;
        transfer.length = 32768;
        transfer.direction = USB_DIR_IN;
        transfer.generation = 0x123;
        CHECK("a data transfer is queued", xhci_submit(hc, 1, 3, &transfer) == 0);
        CHECK("and held", xhci_td_hold(hc, 1, 0x123) == 0);
        xhci_events(hc);
        CHECK("and completes whole",
              xhci_poll(hc, &completion) == 1 && completion.status == USB_XFER_OK);
        transfer.buffer = big + 40960;
        transfer.length = 13;
        transfer.generation = 0x124;
        CHECK("its wrapper is queued on the same endpoint", xhci_submit(hc, 1, 3, &transfer) == 0);
        xhci_events(hc);
        CHECK("and completes with its own generation",
              xhci_poll(hc, &completion) == 1 && completion.generation == 0x124);
        CHECK("while the held record still answers retired", xhci_td_retired(hc, 1, 0x123) == 1);
        xhci_td_release(hc, 1, 0x123);
        CHECK("and answers nothing once released", xhci_td_retired(hc, 1, 0x123) == -1);
    }

    /* The blocks are published whole or not at all: at the first barrier of
       a submission every block but the first is visible and the first is
       not. */
    {
        struct usb_transfer transfer;
        struct usb_completion completion;

        bulk_pair_up();
        transfer.setup = 0;
        transfer.buffer = big + 2048;
        transfer.length = 32768;
        transfer.direction = USB_DIR_IN;
        transfer.generation = 0x112;
        ring_lap_one(3, XHCI_EP_RING_TRBS - 4); /* the transfer crosses the Link */
        watch_ring = hc->slots[1].ring[3];
        watch_first = hc->slots[1].enqueue[3];
        watch_cycle = hc->slots[1].cycle[3];
        watch_blocks = 9;
        watch_result = -1;
        watch_publish = 1;
        CHECK("a transfer is queued under watch", xhci_submit(hc, 1, 3, &transfer) == 0);
        watch_publish = 0;
        CHECK("at its first barrier the first block was not yet valid and the second was",
              watch_result == 1);
        xhci_events(hc);
        CHECK("and it completes",
              xhci_poll(hc, &completion) == 1 && completion.status == USB_XFER_OK);
    }

    if (fails == 0) {
        printf("xhci_hosttest: OK\n");
        return 0;
    }
    printf("xhci_hosttest: %d failures\n", fails);
    return 1;
}
