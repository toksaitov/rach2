/**
 * Host test for the RTL8168GU driver (sys/network/driver/rtl8168.c).
 *
 * host-deps:
 *
 * The chip is not emulated by QEMU or anything else on the development host,
 * so this driver's first execution is on the laptop. That makes this test the
 * only thing standing between a mistake and a machine that cannot be debugged
 * remotely, and it is written to that standard: a software model of the chip
 * answers every register the driver touches with the behavior the reference
 * documents (completion flags that clear, an out-of-band bit that must be
 * cleared, a link list that must be handshaked twice, descriptors whose
 * ownership moves), records every write in order, and the test then asserts
 * the recorded sequence against the reference's own order.
 *
 * What it proves: the bring-up sequence and its order, the four indirect
 * access protocols, the refusal of a chip this driver was not written for,
 * bounded behavior on every handshake that never completes, the descriptor
 * contract in both directions, ring wrap, and the lost-doorbell path.
 *
 * What it cannot prove, and the laptop must: PCIe ordering, whether DMA
 * reaches the rings, the PHY's analog behavior, whether this board needs the
 * firmware patch the port omits, and whether the chip agrees with the
 * reference at all. Returns the failed-check count (0 == pass).
 */

/* The kernel headers the driver includes define uintptr_t themselves, so this
 * test uses no host headers and reports through its exit code, as the other
 * kernel-header tests do. */

/* ------------------------------------------------------------------ */
/* The stubs the driver links against.                                 */
/* ------------------------------------------------------------------ */

/* Recorded delivery from the driver's receive path. */
static unsigned char rx_last[2048];
static unsigned rx_last_len;
static unsigned rx_calls;

/* The interface a frame arrived on, which this model does not look
 * inside. */
struct netdev;

void ethernet_rx(struct netdev *nd, const unsigned char *data, unsigned short len)
{
    (void)nd;
    unsigned i;

    rx_calls++;
    rx_last_len = len;
    for (i = 0; i < len && i < sizeof(rx_last); i++) {
        rx_last[i] = data[i];
    }
}

void ethernet_tx(struct netdev *out, const unsigned char *dst, unsigned short type,
                 const unsigned char *payload, unsigned short plen)
{
    (void)out;
    (void)dst;
    (void)type;
    (void)payload;
    (void)plen;
}

/* The refusal counters the drivers report to. The interface owns them; here
   they are counted so a check can say which one the driver reached. */
static unsigned note_rx_errors;
static unsigned note_rx_drops;

void ethernet_note_rx_error(struct netdev *nd)
{
    (void)nd;
    note_rx_errors++;
}

void ethernet_note_rx_drop(struct netdev *nd)
{
    (void)nd;
    note_rx_drops++;
}

/* The kernel's interrupt bracket. There are no interrupts here, so the flag is
   a token; what the test holds the driver to is that every lock it takes it
   releases, which the balance below checks. */
static int lock_depth;
static int lock_ever_negative;

unsigned int net_lock(void)
{
    lock_depth++;
    return 0;
}

void net_unlock(unsigned int flags)
{
    (void)flags;
    lock_depth--;
    if (lock_depth < 0) {
        lock_ever_negative = 1;
    }
}

void k_puts(const char *s)
{
    (void)s;
}

/* The driver's real delay reads the 8254; the model supplies its own, so this
 * exists only to satisfy the link. */
void k_udelay(unsigned int usec)
{
    (void)usec;
}

void k_printf(const char *fmt, ...)
{
    (void)fmt;
}

unsigned char inb(unsigned short port)
{
    (void)port;
    return 0;
}

void outb(unsigned short port, unsigned char v)
{
    (void)port;
    (void)v;
}

/* The PCI surface the probe uses. The model presents one RTL8168 whose first
 * memory window is the second BAR, so the probe's BAR walk has to step over an
 * I/O BAR to find it, exactly as it will on the laptop. */
static int pci_present = 1;

#include "pci.h"
#include "network/driver/rtl8168.h"

/* The driver reports what it found through the kernel log, which this test
   has no use for; the records are dropped. */
void klog(int level, const char *subsystem, const char *format, ...)
{
    (void)level;
    (void)subsystem;
    (void)format;
}

int pci_find_device_by_id(unsigned short vendor, unsigned short device, pci_device_t *out)
{
    int i;

    if (!pci_present || vendor != 0x10EC || device != 0x8168) {
        return 0;
    }
    out->vendor_id = vendor;
    out->device_id = device;
    out->bus = 1;
    out->device = 0;
    out->function = 0;
    out->irq = 11;
    for (i = 0; i < 6; i++) {
        out->bar[i] = 0;
    }
    out->bar[0] = 0x00003001u; /* an I/O BAR, which must be stepped over */
    out->bar[1] = 0xC0500000u; /* the memory window */
    return 1;
}

int pci_bar_is_memory(unsigned int bar)
{
    return bar != 0 && (bar & 1u) == 0;
}

int pci_bar_is_64bit(unsigned int bar)
{
    return (bar & 0x6u) == 0x4u;
}

unsigned int pci_bar_address(unsigned int bar)
{
    return pci_bar_is_memory(bar) ? (bar & ~0xfu) : (bar & ~0x3u);
}

static int pci_master_enabled;
static int pci_memory_enabled;

/* The PCIe capability the link-state disable walks to, and the config-space
 * words it writes. The model presents a card whose link control has both ASPM
 * bits set, which is the state the target machine's previous operating system
 * leaves, and a bridge above it in the same state. */
#define MODEL_CAP_OFF 0x40
static unsigned short model_link_ctl_dev = 0x0003;
static unsigned short model_link_ctl_bridge = 0x0003;
static int model_bridge_present = 1;

unsigned char pci_read_config8(unsigned char bus, unsigned char device, unsigned char function,
                               unsigned char offset)
{
    (void)device;
    (void)function;
    if (offset == PCI_CAP_PTR) {
        return MODEL_CAP_OFF;
    }
    if (offset == MODEL_CAP_OFF) {
        return PCI_CAP_ID_PCIE;
    }
    if (offset == MODEL_CAP_OFF + 1) {
        return 0; /* end of the capability list */
    }
    if (offset == 0x19) {
        return 1; /* the bridge's secondary bus is the card's bus */
    }
    (void)bus;
    return 0;
}

unsigned short pci_read_config16(unsigned char bus, unsigned char device, unsigned char function,
                                 unsigned char offset)
{
    (void)device;
    (void)function;
    if (offset == PCI_STATUS) {
        return PCI_STATUS_CAP_LIST;
    }
    if (offset == MODEL_CAP_OFF + PCIE_LINK_CONTROL) {
        return bus == 1 ? model_link_ctl_dev : model_link_ctl_bridge;
    }
    return 0;
}

void pci_write_config16(unsigned char bus, unsigned char device, unsigned char function,
                        unsigned char offset, unsigned short value)
{
    (void)device;
    (void)function;
    if (offset == MODEL_CAP_OFF + PCIE_LINK_CONTROL) {
        if (bus == 1) {
            model_link_ctl_dev = value;
        } else {
            model_link_ctl_bridge = value;
        }
    }
}

unsigned char pci_find_capability(unsigned char bus, unsigned char device, unsigned char function,
                                  unsigned char cap_id)
{
    (void)bus;
    (void)device;
    (void)function;
    return cap_id == PCI_CAP_ID_PCIE ? MODEL_CAP_OFF : 0;
}

int pci_find_bridge_for_bus(unsigned char bus, pci_device_t *out)
{
    (void)bus;
    if (!model_bridge_present) {
        return 0;
    }
    out->bus = 0;
    out->device = 28;
    out->function = 0;
    return 1;
}

void pci_enable_bus_master(pci_device_t *dev)
{
    (void)dev;
    pci_master_enabled = 1;
}

void pci_enable_memory_space(pci_device_t *dev)
{
    (void)dev;
    pci_memory_enabled = 1;
}

void pci_disable_bus_master(pci_device_t *dev)
{
    (void)dev;
    pci_master_enabled = 0;
}

unsigned int map_mmio(unsigned int phys, unsigned int size)
{
    (void)size;
    return phys;
}

unsigned char net_mac[6];

/* ------------------------------------------------------------------ */
/* The model of the chip.                                              */
/* ------------------------------------------------------------------ */

#define MODEL_REGS 256

static unsigned char model_reg[MODEL_REGS];

/* What the model does out of the ordinary, so a test can break one thing at a
 * time and watch the driver refuse, not hang. */
static int model_stuck_eri;      /* the ERI completion flag never moves */
static int model_stuck_ocp;      /* the PHY-OCP completion flag never moves */
static int model_stuck_linklist; /* the link list never reports ready */
static int model_stuck_reset;    /* the reset bit never clears */
static int model_stuck_fifo;     /* the FIFOs never report empty */
/* Only the receive FIFO reports empty, the transmit one never does. A chip
   half way through draining is what a wait on "any bit of the mask" would
   accept and a wait on "all of it" refuses, which is the difference the
   reference's own condition makes. */
static int model_half_fifo;
static int model_swallow_doorbell;                 /* the doorbell does nothing */
static unsigned model_xid_txconfig = 0x509u << 20; /* the revision reported */

/* The recorded write sequence. */
struct wr {
    unsigned char width;
    unsigned off;
    unsigned value;
};

#define MODEL_LOG_MAX 512
static struct wr model_log[MODEL_LOG_MAX];
static unsigned model_log_len;

static void model_record(unsigned char width, unsigned off, unsigned value)
{
    if (model_log_len < MODEL_LOG_MAX) {
        model_log[model_log_len].width = width;
        model_log[model_log_len].off = off;
        model_log[model_log_len].value = value;
    }
    model_log_len++;
}

static unsigned model_r32_raw(unsigned off)
{
    return (unsigned)model_reg[off] | ((unsigned)model_reg[off + 1] << 8) |
           ((unsigned)model_reg[off + 2] << 16) | ((unsigned)model_reg[off + 3] << 24);
}

static void model_w32_raw(unsigned off, unsigned v)
{
    model_reg[off] = (unsigned char)v;
    model_reg[off + 1] = (unsigned char)(v >> 8);
    model_reg[off + 2] = (unsigned char)(v >> 16);
    model_reg[off + 3] = (unsigned char)(v >> 24);
}

static unsigned char m_r8(unsigned off)
{
    if (off == R8168_CHIPCMD) {
        /* The reset bit clears itself unless the test holds it. */
        if (!model_stuck_reset) {
            model_reg[off] = (unsigned char)(model_reg[off] & ~R8168_CMD_RESET);
        }
        return model_reg[off];
    }
    if (off == R8168_MCU) {
        unsigned char v = model_reg[off];

        if (model_half_fifo) {
            v = (unsigned char)(v | (R8168_MCU_RXTX_EMPTY & 0x10));
        } else if (!model_stuck_fifo) {
            v = (unsigned char)(v | R8168_MCU_RXTX_EMPTY);
        }
        if (!model_stuck_linklist) {
            v = (unsigned char)(v | R8168_MCU_LINK_LIST_RDY);
        }
        return v;
    }
    return model_reg[off];
}

static unsigned short m_r16(unsigned off)
{
    return (unsigned short)((unsigned)model_reg[off] | ((unsigned)model_reg[off + 1] << 8));
}

static unsigned m_r32(unsigned off)
{
    if (off == R8168_TXCONFIG) {
        unsigned v = model_r32_raw(off) & 0x000fffffu;

        v |= model_xid_txconfig;
        if (!model_stuck_fifo) {
            v |= R8168_TXCFG_EMPTY;
        }
        return v;
    }
    /* Every window register reads back what its completed command left: the
       write handler below is where a command completes. */
    return model_r32_raw(off);
}

/* The register files the model keeps behind the four indirect windows. */
static unsigned model_eri[0x400];
static unsigned short model_phy[0x10000];
static unsigned short model_ephy[32];
static unsigned model_csi[0x1000];
static unsigned short model_mac_ocp[0x10000];

/* The chip's transmit engine, defined once the driver's rings are in scope. */
static void model_tx_drain(void);

static void m_w8(unsigned off, unsigned char v)
{
    model_record(8, off, v);
    if (off == R8168_TXPOLL) {
        if (model_swallow_doorbell) {
            return; /* a poll request the chip lost */
        }
        /* The chip's transmit engine: every descriptor it owns is sent and
           handed back. This is the model standing in for the DMA the driver
           is waiting on. */
        model_tx_drain();
        return;
    }
    model_reg[off] = v;
}

static void m_w16(unsigned off, unsigned short v)
{
    model_record(16, off, v);
    model_reg[off] = (unsigned char)v;
    model_reg[off + 1] = (unsigned char)(v >> 8);
}

static void m_w32(unsigned off, unsigned v)
{
    model_record(32, off, v);
    model_w32_raw(off, v);

    /* A command completes here. A write completes by its flag going low, a
       read by the flag going high with the data readable; a window the test
       has stuck never reaches the state its caller polls for. */
    if (off == R8168_OCPDR) {
        /* The command word has the register in bits 30:16 as reg/2 (the
           driver forms it as reg << 15 and the register is always even), so
           the data's top bit does not reach the address field. */
        unsigned reg = ((v >> 16) & 0x7fffu) << 1;

        if (v & R8168_OCP_FLAG) {
            model_mac_ocp[reg] = (unsigned short)(v & 0xffffu);
        } else {
            model_w32_raw(off, (v & ~0xffffu) | model_mac_ocp[reg]);
        }
    } else if (off == R8168_ERIAR) {
        unsigned addr = v & 0xfffu;

        if (v & R8168_ERIAR_WRITE) {
            unsigned data = model_r32_raw(R8168_ERIDR);
            unsigned mask = 0;

            if (v & 0x1000u) {
                mask |= 0x000000ffu;
            }
            if (v & 0x2000u) {
                mask |= 0x0000ff00u;
            }
            if (v & 0x4000u) {
                mask |= 0x00ff0000u;
            }
            if (v & 0x8000u) {
                mask |= 0xff000000u;
            }
            model_eri[addr & 0x3ff] = (model_eri[addr & 0x3ff] & ~mask) | (data & mask);
            model_w32_raw(off, model_stuck_eri ? (v | R8168_ERIAR_FLAG) : (v & ~R8168_ERIAR_FLAG));
        } else {
            model_w32_raw(R8168_ERIDR, model_eri[addr & 0x3ff]);
            model_w32_raw(off, model_stuck_eri ? (v & ~R8168_ERIAR_FLAG) : (v | R8168_ERIAR_FLAG));
        }
    } else if (off == R8168_GPHY_OCP) {
        unsigned reg = ((v >> 16) & 0x7fffu) << 1;

        if (v & R8168_OCP_FLAG) {
            /* A PHY reset bit clears itself, as the hardware's does. */
            model_phy[reg] = (unsigned short)((v & 0xffffu) & ~0x8000u);
            model_w32_raw(off, model_stuck_ocp ? (v | R8168_OCP_FLAG) : (v & ~R8168_OCP_FLAG));
        } else {
            unsigned answer = (v & ~0xffffu) | model_phy[reg];

            model_w32_raw(off,
                          model_stuck_ocp ? (answer & ~R8168_OCP_FLAG) : (answer | R8168_OCP_FLAG));
        }
    } else if (off == R8168_EPHYAR) {
        unsigned reg = (v >> 16) & 0x1fu;

        if (v & R8168_ERIAR_FLAG) {
            model_ephy[reg] = (unsigned short)(v & 0xffffu);
            model_w32_raw(off, v & ~R8168_ERIAR_FLAG);
        } else {
            model_w32_raw(off, (v & ~0xffffu) | model_ephy[reg] | R8168_ERIAR_FLAG);
        }
    } else if (off == R8168_CSIAR) {
        unsigned addr = v & 0xfffu;

        if (v & R8168_CSIAR_FLAG) {
            model_csi[addr] = model_r32_raw(R8168_CSIDR);
            model_w32_raw(off, v & ~R8168_CSIAR_FLAG);
        } else {
            model_w32_raw(R8168_CSIDR, model_csi[addr]);
            model_w32_raw(off, v | R8168_CSIAR_FLAG);
        }
    }
}

static void m_udelay(unsigned usec)
{
    (void)usec;
}

/* ------------------------------------------------------------------ */

#include "network/driver/rtl8168.c"

static int fails;

/* Declared, not included: the kernel's kint.h and the host's headers
 * disagree about uintptr_t, so this test pulls in no host header and names the
 * one function it needs to say which check failed. */
extern int printf(const char *fmt, ...);

#define CHECK(cond, what)                   \
    do {                                    \
        if (!(cond)) {                      \
            fails++;                        \
            printf("  FAIL: %s\n", (what)); \
        }                                   \
    } while (0)

static const struct r8168_io model_io = {m_r8, m_r16, m_r32, m_w8, m_w16, m_w32, m_udelay};

/**
 * Send every transmit descriptor the chip owns and hand it back.
 *
 * The doorbell is what starts this on real hardware, so the model does it
 * from the doorbell write: without it the driver would wait for an ownership
 * bit nothing ever clears, which is exactly what the stall case tests.
 */
static void model_tx_drain(void)
{
    unsigned n;

    for (n = 0; n < R8168_RING_SIZE; n++) {
        if (r8168_tx_ring[n].opts1 & R8168_DESC_OWN) {
            r8168_tx_ring[n].opts1 &= ~R8168_DESC_OWN;
        }
    }
}

/** Put the model and the driver back to a cold-power state. */
static void model_reset(void)
{
    unsigned i;

    for (i = 0; i < MODEL_REGS; i++) {
        model_reg[i] = 0;
    }
    for (i = 0; i < 0x400; i++) {
        model_eri[i] = 0;
    }
    for (i = 0; i < 32; i++) {
        model_ephy[i] = 0;
    }
    for (i = 0; i < 0x1000; i++) {
        model_csi[i] = 0;
    }
    for (i = 0; i < 0x10000; i++) {
        model_mac_ocp[i] = 0;
    }
    for (i = 0; i < 0x10000; i++) {
        model_phy[i] = 0;
    }
    model_log_len = 0;
    model_stuck_eri = 0;
    model_stuck_ocp = 0;
    model_stuck_linklist = 0;
    model_stuck_reset = 0;
    model_stuck_fifo = 0;
    model_half_fifo = 0;
    model_swallow_doorbell = 0;
    model_xid_txconfig = 0x509u << 20;
    pci_present = 1;
    pci_master_enabled = 0;
    pci_memory_enabled = 0;
    note_rx_errors = 0;
    note_rx_drops = 0;
    rx_calls = 0;
    rx_last_len = 0;
    r8168_up = 0;
    r8168_ocp_base = R8168_OCP_STD_PHY_BASE;
    r8168_rx_packets = 0;
    r8168_rx_errors = 0;
    r8168_rx_dropped = 0;
    r8168_tx_packets = 0;
    r8168_tx_stalls = 0;
    io = &model_io;
    /* The chip does not arrive from a cold power-on: it arrives from another
     * operating system, so every register the driver reads is seeded with a
     * value that is wrong for us. A driver that skips a write, or preserves
     * bits it should clear, then fails a check it would pass against a
     * register file of zeroes. */
    for (i = 0; i < MODEL_REGS; i++) {
        model_reg[i] = 0xA5;
    }
    for (i = 0; i < 0x400; i++) {
        model_eri[i] = 0xA5A5A5A5u;
    }
    for (i = 0; i < 32; i++) {
        model_ephy[i] = 0xA5A5;
    }
    model_reg[R8168_MCU] = R8168_MCU_NOW_IS_OOB;
    /* CPlusCmd arrives with every bit set, so a wrong preserve mask shows. */
    model_reg[R8168_CPLUSCMD] = 0xFF;
    model_reg[R8168_CPLUSCMD + 1] = 0xFF;
    /* The permanent station address the chip was built with, which is where
     * the driver must take it from, not from the register pair. */
    model_eri[0xe0] = 0x7E44D228u;
    model_eri[0xe4] = 0x0000AFE0u;
    /* The register pair has a plausible but WRONG address, so a driver that
     * reads it instead of the permanent one is visible. */
    model_w32_raw(R8168_MAC0, 0xDEADBEEFu);
    model_w32_raw(R8168_MAC4, 0x0000BEEFu);
    model_link_ctl_dev = 0x0003;
    model_link_ctl_bridge = 0x0003;
    model_bridge_present = 1;
}

/** The index of the first write of `width` to `off` at or after `from`, or
 * -1. */
static int find_write(unsigned from, unsigned char width, unsigned off)
{
    unsigned i;

    for (i = from; i < model_log_len && i < MODEL_LOG_MAX; i++) {
        if (model_log[i].width == width && model_log[i].off == off) {
            return (int)i;
        }
    }
    return -1;
}

/** As find_write, but the value must match too. */
static int find_write_value(unsigned from, unsigned char width, unsigned off, unsigned value)
{
    unsigned i;

    for (i = from; i < model_log_len && i < MODEL_LOG_MAX; i++) {
        if (model_log[i].width == width && model_log[i].off == off && model_log[i].value == value) {
            return (int)i;
        }
    }
    return -1;
}

/** The index of the ERI command word that writes `addr`, or -1. */
static int find_eri_write(unsigned from, unsigned addr)
{
    unsigned i;

    for (i = from; i < model_log_len && i < MODEL_LOG_MAX; i++) {
        if (model_log[i].width == 32 && model_log[i].off == R8168_ERIAR &&
            (model_log[i].value & R8168_ERIAR_WRITE) && (model_log[i].value & 0xfffu) == addr) {
            return (int)i;
        }
    }
    return -1;
}

/* The interface this model's card is brought up for. */
static struct netdev model_iface;

int main(void)
{
    int a, b, c, d, e;
    unsigned i;

    /* ---- the bring-up completes on a healthy model ---- */
    model_reset();
    {
        int rc = rtl8168_init(&model_iface, 0);

        if (rc != 1) {
            printf("  init refused: %s\n", r8168_err);
        }
        CHECK(rc == NETDEV_UP, "the bring-up completes");
    }
    CHECK(r8168_up == 1, "the driver reports itself up");
    CHECK(pci_memory_enabled && pci_master_enabled, "decoding and bus mastering are enabled");
    CHECK((model_link_ctl_dev & PCIE_LINK_CTL_ASPM_MASK) == 0,
          "the card's own link state is turned off");
    CHECK((model_link_ctl_bridge & PCIE_LINK_CTL_ASPM_MASK) == 0,
          "the link state is turned off at the port above the card too");
    CHECK(r8168_xid == 0x509u, "the revision was read from the chip");
    CHECK(net_mac[0] == 0x28 && net_mac[1] == 0xD2 && net_mac[2] == 0x44 && net_mac[3] == 0x7E &&
              net_mac[4] == 0xE0 && net_mac[5] == 0xAF,
          "the station address came from the permanent one, not the register pair");
    CHECK((model_reg[R8168_CPLUSCMD] | (model_reg[R8168_CPLUSCMD + 1] << 8)) ==
              (0xFFFFu & R8168_CPCMD_MASK),
          "CPlusCmd kept exactly the bits the reference preserves");

    /* ---- the identify and quiesce order ---- */
    /* The out-of-band bit must be cleared, and the link list handshaked with
       one bit cleared and then another set, before any reset. */
    a = find_write(0, 8, R8168_MCU);
    CHECK(a >= 0 && (model_log[a].value & R8168_MCU_NOW_IS_OOB) == 0,
          "the out-of-band bit is cleared");
    CHECK((model_mac_ocp[0xe8de] & (1u << 15)) != 0, "the second link-list handshake set its bit");
    CHECK((model_mac_ocp[0xe8de] & (1u << 14)) == 0,
          "the first link-list handshake cleared its bit");
    b = find_write(0, 32, R8168_OCPDR);
    CHECK(b > a, "the link-list handshake follows the out-of-band clear");
    c = find_write_value(0, 8, R8168_CHIPCMD, R8168_CMD_RESET);
    CHECK(c > b, "the reset follows the handshake");

    /* ---- the receive gate is closed before the reset and opened before the
           receiver is enabled ---- */
    a = find_write(0, 32, R8168_MISC);
    CHECK(a >= 0 && (model_log[a].value & R8168_MISC_RXDV_GATED) != 0,
          "the receive gate is closed first");
    b = find_write_value(0, 8, R8168_CHIPCMD, R8168_CMD_TX_ENB | R8168_CMD_RX_ENB);
    CHECK(b >= 0, "the receiver and transmitter are enabled");
    {
        int gate_open = -1;

        for (i = 0; i < model_log_len; i++) {
            if (model_log[i].width == 32 && model_log[i].off == R8168_MISC &&
                (model_log[i].value & R8168_MISC_RXDV_GATED) == 0) {
                gate_open = (int)i;
            }
        }
        CHECK(gate_open >= 0 && gate_open < b,
              "the receive gate is open before the receiver is enabled");
    }

    /* ---- the start sequence, in the reference's order ---- */
    {
        int unlock = -1, cfg2 = -1, cpluscmd = -1, maxtx = -1, ephy_last = -1;
        int lock = -1, rxcfg = -1, txcfg = -1, intrmask = -1, ringlo = -1, ringhi = -1;

        /* Search from the last unlock, which is the start sequence's own. */
        for (i = 0; i < model_log_len; i++) {
            if (model_log[i].width == 8 && model_log[i].off == R8168_CFG9346 &&
                model_log[i].value == R8168_CFG9346_UNLOCK) {
                unlock = (int)i;
            }
        }
        CHECK(unlock >= 0, "the configuration registers are unlocked");
        cfg2 = find_write((unsigned)unlock, 8, R8168_CONFIG2);
        cpluscmd = find_write((unsigned)unlock, 16, R8168_CPLUSCMD);
        maxtx = find_write((unsigned)unlock, 8, R8168_MAXTXPACKETSIZE);
        CHECK(cfg2 > unlock, "clock request is disabled inside the window");
        CHECK(cpluscmd > cfg2, "CPlusCmd follows the power-management writes");
        CHECK(maxtx > cpluscmd, "the early-transmit size follows CPlusCmd");

        /* The chip-specific table, then the EPHY table, then the ring
           addresses, then the lock: the reference's order. */
        a = find_eri_write((unsigned)maxtx, 0xc8);
        b = find_eri_write((unsigned)maxtx, 0xe8);
        CHECK(a > maxtx && b > a, "the FIFO sizes are written in order");
        c = find_eri_write((unsigned)maxtx, 0x2f8);
        CHECK(c > b, "the 0x2f8 write follows the FIFO sizes");
        for (i = 0; i < model_log_len; i++) {
            if (model_log[i].width == 32 && model_log[i].off == R8168_EPHYAR &&
                (model_log[i].value & R8168_ERIAR_FLAG)) {
                ephy_last = (int)i;
            }
        }
        CHECK(ephy_last > c, "the EPHY table follows the chip configuration");
        ringhi = find_write((unsigned)ephy_last, 32, R8168_TXDESC_HI);
        ringlo = find_write((unsigned)ephy_last, 32, R8168_TXDESC_LO);
        CHECK(ringhi > ephy_last && ringlo > ringhi,
              "the ring address high half is written before the low half");
        lock = find_write_value((unsigned)ringlo, 8, R8168_CFG9346, R8168_CFG9346_LOCK);
        CHECK(lock > ringlo, "the configuration window closes after the rings");
        rxcfg = find_write_value((unsigned)lock, 32, R8168_RXCONFIG, R8168_RXCFG_VALUE);
        txcfg = find_write_value((unsigned)lock, 32, R8168_TXCONFIG, R8168_TXCFG_VALUE);
        CHECK(rxcfg > lock && txcfg > rxcfg,
              "the receive and transmit configurations follow the lock");
        intrmask = find_write_value((unsigned)txcfg, 16, R8168_INTRMASK, 0);
        CHECK(intrmask > txcfg, "the interrupt mask is left at zero");
    }

    /* ---- the exact values the reference writes ---- */
    CHECK(find_write_value(0, 32, R8168_TXCONFIG, R8168_TXCFG_VALUE) >= 0,
          "TxConfig takes the reference's value");
    CHECK(find_write_value(0, 32, R8168_RXCONFIG, R8168_RXCFG_VALUE) >= 0,
          "RxConfig takes the reference's value");
    CHECK(find_write_value(0, 16, R8168_RXMAXSIZE, R8168_RX_MAX_SIZE) >= 0,
          "the maximum receive size is the buffer size plus one");
    CHECK(find_write_value(0, 8, R8168_MAXTXPACKETSIZE, 0x27) >= 0,
          "the early-transmit threshold is the reference's");
    CHECK(find_write_value(0, 16, R8168_INTRMITIGATE, 0) >= 0, "interrupt coalescing is off");
    CHECK(model_eri[0xc8] == 0x00080002u && model_eri[0xe8] == 0x00100006u,
          "the FIFO sizes reached the chip");
    CHECK((model_eri[0xcc] & 0xff) == 0x38 && (model_eri[0xd0] & 0xff) == 0x48,
          "the pause thresholds reached the chip");
    CHECK((model_eri[0x2f8] & 0xffff) == 0x1d8f, "the 0x2f8 value reached the chip");
    CHECK(model_eri[0xdc] & 1u,
          "the packet filter ends set, because the reference clears then sets it");
    CHECK((model_eri[0x1b0] & 0x3u) == 0x3u && (model_eri[0x1b0] & 0x1000u) == 0,
          "both read-modify-writes of 0x1b0 landed");
    CHECK((model_eri[0x2fc] & 0x7u) == 0x1u, "the 0x2fc bits landed");
    CHECK(model_ephy[0x19] == 0x7c00 && model_ephy[0x1e] == 0x20eb && model_ephy[0x0d] == 0x1666 &&
              model_ephy[0x00] == 0x10a3 && model_ephy[0x06] == 0xf050,
          "the EPHY table landed");
    CHECK((model_ephy[0x04] & 0x0010) == 0x0010, "the EPHY read-modify-write landed");

    /* ---- the ring contract ---- */
    CHECK((r8168_rx_ring[0].opts1 & R8168_DESC_OWN) != 0,
          "every receive descriptor is owned by the chip");
    CHECK((r8168_rx_ring[0].opts1 & R8168_DESC_LEN_MASK) == R8168_RX_BUF_SIZE,
          "a receive descriptor has the buffer size");
    CHECK((r8168_rx_ring[R8168_RING_SIZE - 1].opts1 & R8168_DESC_EOR) != 0,
          "the last receive descriptor ends the ring");
    CHECK((r8168_rx_ring[0].opts1 & R8168_DESC_EOR) == 0,
          "no other receive descriptor ends the ring");

    /* ---- the chip this driver refuses ---- */
    model_reset();
    model_xid_txconfig = 0x5c8u << 20; /* an 8168 that is not the GU */
    CHECK(rtl8168_init(&model_iface, 0) == NETDEV_FAILED, "a different 8168 is refused");
    CHECK(r8168_up == 0, "the driver stays down after a refusal");

    /* An identifier differing only in the bits the family mask ignores is the
       same chip and must be accepted. */
    model_reset();
    model_xid_txconfig = (0x509u | 0x800u) << 20;
    CHECK(rtl8168_init(&model_iface, 0) == NETDEV_UP,
          "an identifier differing outside the mask is accepted");

    /* A window that reads back all ones is an absent device, not a chip. */
    model_reset();
    model_xid_txconfig = 0xfff00000u;
    CHECK(rtl8168_init(&model_iface, 0) == NETDEV_FAILED, "an all-ones window is refused");

    model_reset();
    pci_present = 0;
    CHECK(rtl8168_init(&model_iface, 0) == NETDEV_ABSENT,
          "an absent card reports absence, not failure");

    /* ---- every handshake that never completes is bounded ---- */
    model_reset();
    model_stuck_fifo = 1;
    CHECK(rtl8168_init(&model_iface, 0) == NETDEV_FAILED,
          "a chip whose FIFOs never drain is refused");

    model_reset();
    model_half_fifo = 1;
    CHECK(rtl8168_init(&model_iface, 0) == NETDEV_FAILED,
          "a chip with one FIFO still holding data is refused, not accepted as drained");

    model_reset();
    model_stuck_linklist = 1;
    CHECK(rtl8168_init(&model_iface, 0) == NETDEV_FAILED,
          "a link list that never reports ready is refused");

    model_reset();
    model_stuck_reset = 1;
    CHECK(rtl8168_init(&model_iface, 0) == NETDEV_FAILED,
          "a reset that never completes is refused");

    model_reset();
    model_stuck_eri = 1;
    CHECK(rtl8168_init(&model_iface, 0) == NETDEV_FAILED,
          "an ERI window that never completes is refused");

    model_reset();
    model_stuck_ocp = 1;
    CHECK(rtl8168_init(&model_iface, 0) == NETDEV_FAILED,
          "a PHY window that never completes is refused");

    /* ---- transmit ---- */
    model_reset();
    CHECK(rtl8168_init(&model_iface, 0) == NETDEV_UP,
          "the bring-up completes before the transmit cases");
    {
        unsigned char frame[100];
        unsigned before = model_log_len;
        int doorbell;

        for (i = 0; i < sizeof(frame); i++) {
            frame[i] = (unsigned char)i;
        }
        rtl8168_tx(model_iface.dev, frame, (unsigned short)sizeof(frame));
        CHECK(r8168_tx_packets == 1, "the frame was sent");
        CHECK(r8168_tx_stalls == 0, "the frame did not stall");
        doorbell = find_write_value(before, 8, R8168_TXPOLL, R8168_TXPOLL_NPQ);
        CHECK(doorbell >= 0, "the doorbell was rung");
        CHECK((r8168_tx_ring[0].opts1 & (R8168_DESC_FS | R8168_DESC_LS)) ==
                  (R8168_DESC_FS | R8168_DESC_LS),
              "the descriptor marks one whole frame");
        CHECK((r8168_tx_ring[0].opts1 & R8168_DESC_LEN_MASK) == sizeof(frame),
              "the descriptor has the frame length");
        CHECK(r8168_tx_buf[0][0] == 0 && r8168_tx_buf[0][99] == 99,
              "the frame was copied into the buffer");
    }

    /* A frame under the Ethernet minimum is padded, and the padding is zero,
     * not whatever the buffer held. */
    {
        unsigned char frame[20];
        unsigned char dirty = 0xAA;

        for (i = 0; i < sizeof(r8168_tx_buf[1]); i++) {
            r8168_tx_buf[1][i] = dirty;
        }
        for (i = 0; i < sizeof(frame); i++) {
            frame[i] = 0x11;
        }
        rtl8168_tx(model_iface.dev, frame, (unsigned short)sizeof(frame));
        CHECK((r8168_tx_ring[1].opts1 & R8168_DESC_LEN_MASK) == 60,
              "a short frame is padded to the Ethernet minimum");
        CHECK(r8168_tx_buf[1][59] == 0, "the padding is zero, not stale bytes");
    }

    /* A chip that never takes the frame is counted and gives up: the machine
       must not stop because the wire did. */
    model_reset();
    CHECK(rtl8168_init(&model_iface, 0) == NETDEV_UP,
          "the bring-up completes before the stall case");
    model_swallow_doorbell = 1;
    {
        unsigned char frame[64];
        unsigned f;

        for (i = 0; i < sizeof(frame); i++) {
            frame[i] = 1;
        }
        /* A chip that never answers the doorbell keeps every descriptor. The
           transmit is asynchronous, so the ring absorbs the first RING_SIZE
           frames and only then is there nowhere to put one; that is the
           backpressure a burst meets, and it must be a counted refusal rather
           than a wait without end. */
        for (f = 0; f < R8168_RING_SIZE; f++) {
            rtl8168_tx(model_iface.dev, frame, (unsigned short)sizeof(frame));
        }
        CHECK(r8168_tx_packets == R8168_RING_SIZE, "the ring absorbs a full burst");
        CHECK(r8168_tx_stalls == 0, "and nothing stalls while there is room");
        rtl8168_tx(model_iface.dev, frame, (unsigned short)sizeof(frame));
        CHECK(r8168_tx_stalls == 1, "the frame past a full ring is counted, not waited on");
        CHECK(r8168_tx_packets == R8168_RING_SIZE, "and is not counted as sent");

        /* The tick rings the doorbell again for whatever is outstanding, which
           is what recovers a poll request the chip lost. */
        {
            unsigned before = model_log_len;

            rtl8168_poll(model_iface.dev);
            CHECK(find_write(before, 8, R8168_TXPOLL) >= 0,
                  "the tick rings the doorbell again while frames are outstanding");
        }
    }

    /* ---- a working chip drains, and the ring wraps under it ---- */
    model_reset();
    CHECK(rtl8168_init(&model_iface, 0) == NETDEV_UP,
          "the bring-up completes before the drain case");
    {
        unsigned char frame[64];
        unsigned f;

        for (i = 0; i < sizeof(frame); i++) {
            frame[i] = 2;
        }
        for (f = 0; f < R8168_RING_SIZE * 3; f++) {
            rtl8168_tx(model_iface.dev, frame, (unsigned short)sizeof(frame));
        }
        CHECK(r8168_tx_packets == R8168_RING_SIZE * 3,
              "a chip that answers takes three ring loads without a stall");
        CHECK(r8168_tx_stalls == 0, "and stalls on none of them");
        rtl8168_poll(model_iface.dev);
        CHECK(r8168_tx_tail == r8168_tx_head, "the tick reclaims every completed descriptor");
    }

    /* ---- receive ---- */
    model_reset();
    CHECK(rtl8168_init(&model_iface, 0) == NETDEV_UP,
          "the bring-up completes before the receive cases");
    {
        unsigned len = 64;

        for (i = 0; i < len; i++) {
            r8168_rx_buf[0][i] = (unsigned char)(i + 3);
        }
        /* The chip hands the descriptor back with the length INCLUDING the
           frame check sequence. */
        r8168_rx_ring[0].opts1 = R8168_DESC_FS | R8168_DESC_LS | (len + 4);
        rtl8168_poll(model_iface.dev);
        CHECK(rx_calls == 1, "the frame was delivered");
        CHECK(rx_last_len == len, "the frame check sequence was stripped");
        CHECK(rx_last[0] == 3 && rx_last[63] == 66, "the payload was delivered");
        CHECK((r8168_rx_ring[0].opts1 & R8168_DESC_OWN) != 0,
              "the descriptor was returned to the chip");
        CHECK(r8168_rx_packets == 1, "the receive was counted");
    }

    /* An error descriptor is counted and not delivered. */
    {
        unsigned before = rx_calls;

        r8168_rx_ring[1].opts1 = R8168_DESC_FS | R8168_DESC_LS | R8168_DESC_RX_RES | 64;
        rtl8168_poll(model_iface.dev);
        CHECK(rx_calls == before, "an error frame is not delivered");
        CHECK(r8168_rx_errors == 1, "an error frame is counted");
    }

    /* A frame split across descriptors is dropped rather than
     * half-delivered. */
    {
        unsigned before = rx_calls;

        r8168_rx_ring[2].opts1 = R8168_DESC_FS | 64;
        rtl8168_poll(model_iface.dev);
        CHECK(rx_calls == before, "a fragment is not delivered");
        CHECK(r8168_rx_dropped == 1, "a fragment is counted as dropped");
    }

    /* A length the buffer cannot hold is refused. */
    {
        unsigned before = rx_calls;

        r8168_rx_ring[3].opts1 = R8168_DESC_FS | R8168_DESC_LS | 2;
        rtl8168_poll(model_iface.dev);
        CHECK(rx_calls == before, "a frame shorter than its checksum is not delivered");
        CHECK(r8168_rx_dropped == 2, "the short frame is counted as dropped");
    }

    /* ---- the ring wraps, and a full ring is drained in one pass ---- */
    model_reset();
    CHECK(rtl8168_init(&model_iface, 0) == NETDEV_UP,
          "the bring-up completes before the wrap case");
    {
        unsigned n;

        for (n = 0; n < R8168_RING_SIZE; n++) {
            r8168_rx_ring[n].opts1 = R8168_DESC_FS | R8168_DESC_LS |
                                     ((n == R8168_RING_SIZE - 1) ? R8168_DESC_EOR : 0) | 68;
        }
        rtl8168_poll(model_iface.dev);
        CHECK(rx_calls == R8168_RING_SIZE, "a full ring is drained in one pass");
        CHECK(r8168_rx_cur == R8168_RING_SIZE, "the ring index advanced by the ring size");
        CHECK((r8168_rx_ring[R8168_RING_SIZE - 1].opts1 & R8168_DESC_EOR) != 0,
              "the end-of-ring mark survives the refill");
        CHECK((r8168_rx_ring[0].opts1 & R8168_DESC_EOR) == 0,
              "no other descriptor gained the end-of-ring mark");

        /* The next descriptor the driver looks at is the first one again. */
        rx_calls = 0;
        r8168_rx_ring[0].opts1 = R8168_DESC_FS | R8168_DESC_LS | 68;
        rtl8168_poll(model_iface.dev);
        CHECK(rx_calls == 1, "the ring wrapped back to its first descriptor");
    }

    /* The transmit ring wraps too, and the end-of-ring mark lands only on the
       last descriptor. */
    {
        unsigned char frame[64];
        unsigned n;

        for (i = 0; i < sizeof(frame); i++) {
            frame[i] = 2;
        }
        for (n = 0; n < R8168_RING_SIZE + 2; n++) {
            rtl8168_tx(model_iface.dev, frame, (unsigned short)sizeof(frame));
        }
        CHECK(r8168_tx_packets == R8168_RING_SIZE + 2, "every frame in the wrap was sent");
        CHECK((r8168_tx_ring[R8168_RING_SIZE - 1].opts1 & R8168_DESC_EOR) != 0,
              "the last transmit descriptor ends the ring");
        CHECK((r8168_tx_ring[0].opts1 & R8168_DESC_EOR) == 0,
              "the first transmit descriptor does not");
    }

    /* ---- a driver that is not up does nothing ---- */
    model_reset();
    {
        unsigned char frame[64];

        CHECK(rtl8168_tx(model_iface.dev, frame, (unsigned short)sizeof(frame)) ==
                  NETDEV_TX_DROPPED,
              "a driver that never came up refuses the frame rather than swallowing it");
        rtl8168_poll(model_iface.dev);
        CHECK(r8168_tx_packets == 0 && rx_calls == 0,
              "a driver that never came up neither sends nor receives");
    }

    /* ---- the driver's table ---- */
    CHECK(rtl8168_netdev.tx == rtl8168_tx, "the table names the transmit path");
    CHECK(rtl8168_netdev.poll == rtl8168_poll, "the table names the poll path");
    CHECK(rtl8168_netdev.init == rtl8168_init, "the table names the bring-up");
    CHECK(rtl8168_netdev.error != 0, "the table names a diagnostic");

    CHECK(lock_depth == 0 && !lock_ever_negative,
          "every interrupt bracket the driver took, it released");

    (void)a;
    (void)b;
    (void)c;
    (void)d;
    (void)e;
    return fails;
}
