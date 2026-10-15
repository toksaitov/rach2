/**
 * Test the RTL8168GU driver, sys/network/driver/rtl8168.c, against a software
 * model of the chip.
 *
 * host-deps:
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 *
 * The chip is not emulated by QEMU or anything else on the development host,
 * so this driver's first execution is on the laptop. This test is the only
 * check before a mistake reaches a machine that cannot be debugged remotely,
 * and it is written to that standard: a software model of the chip
 * answers every register the driver touches with the behavior the reference
 * documents (completion flags that clear, an out-of-band bit that must be
 * cleared, a link list that must be handshaked twice, descriptors whose
 * ownership moves), records every write in order, and the test then asserts
 * the recorded sequence against the reference's own order.
 *
 * What it proves: the bring-up sequence and its order, the four indirect
 * access protocols, the rejection of a chip this driver was not written for,
 * bounded behavior on every handshake that never completes, the descriptor
 * contract in both directions, ring wrap, and the lost-doorbell path.
 *
 * What it cannot prove, and the laptop must: PCIe ordering, whether DMA
 * reaches the rings, the PHY's analog behavior, whether this board needs the
 * firmware patch the port omits, and whether the chip behaves as the
 * reference says at all.
 *
 * The test exits with the number of failed checks, at most 255.
 */

/* The kernel headers the driver includes define uintptr_t themselves, so this
 * test uses no host headers and reports through its exit code, as the other
 * kernel-header tests do. */

/* ------------------------------------------------------------------ */
/* The stubs the driver links against.                                 */
/* ------------------------------------------------------------------ */

/* The last frame the driver's receive path delivered, and the deliveries. */
static unsigned char rx_last[2048];
static unsigned rx_last_len;
static unsigned rx_calls;

/* The interface a frame arrived on, which this model does not look
 * inside. */
struct netdev;

static void (*rx_hook)(void); /* what the test does while a frame is handed up */
static unsigned rx_first_at;  /* the model log's length at the first delivery */
static unsigned model_log_len;

/** Record a frame delivered and count it, noting the write log's length at
 * the first delivery and running `rx_hook` before the copy. */
void ethernet_rx(struct netdev *nd, const unsigned char *data, unsigned short len)
{
    (void)nd;
    unsigned i;

    if (rx_calls == 0) {
        rx_first_at = model_log_len;
    }
    rx_calls++;
    rx_last_len = len;
    if (rx_hook) {
        rx_hook();
    }
    for (i = 0; i < len && i < sizeof(rx_last); i++) {
        rx_last[i] = data[i];
    }
}

/** Send nothing and return 0, NETDEV_TX_OK; no row sends through Ethernet. */
int ethernet_tx(struct netdev *out, const unsigned char *dst, unsigned short type,
                const unsigned char *payload, unsigned short plen)
{
    (void)out;
    (void)dst;
    (void)type;
    (void)payload;
    (void)plen;
    return 0;
}

/* The counters of frames the drivers drop or find in error. The interface
   owns them; here they are counted so a check can say which one the driver
   reached. */
static unsigned note_rx_errors;
static unsigned note_rx_drops;

/** Count a frame in error. */
void ethernet_note_rx_error(struct netdev *nd)
{
    (void)nd;
    note_rx_errors++;
}

/** Count a frame dropped. */
void ethernet_note_rx_drop(struct netdev *nd)
{
    (void)nd;
    note_rx_drops++;
}

/* The network lock's balance: locks taken and not released, and whether a
   release ever came without a lock. The test holds the driver to releasing
   every lock it takes. */
static int lock_depth;
static int lock_ever_negative;

/** Take the network lock: count it and return 0 as the saved flags. */
unsigned int net_lock(void)
{
    lock_depth++;
    return 0;
}

/** Release the network lock, noting a release without a lock. */
void net_unlock(unsigned int flags)
{
    (void)flags;
    lock_depth--;
    if (lock_depth < 0) {
        lock_ever_negative = 1;
    }
}

/** Discard console text. */
void k_puts(const char *s)
{
    (void)s;
}

/** Wait: never reached, since the model's `udelay` takes the driver's waits;
 * the kernel's measures against a hardware counter. It completes the link. */
void k_udelay(unsigned int usec)
{
    (void)usec;
}

/** Discard console text. */
void k_printf(const char *fmt, ...)
{
    (void)fmt;
}

/** Read an I/O port as 0. */
unsigned char inb(unsigned short port)
{
    (void)port;
    return 0;
}

/** Write an I/O port: a no-op. */
void outb(unsigned short port, unsigned char v)
{
    (void)port;
    (void)v;
}

/* The PCI calls the probe makes. The model presents one RTL8168 whose first
 * memory window is the second BAR, so the probe's BAR walk has to step over an
 * I/O BAR to find it, exactly as it will on the laptop. */
static int pci_present = 1;

#include "pci.h"
#include "isr.h"
#include "network/driver/rtl8168.h"

/* The format of the last line the driver logged, and the values of a link
   line, for the rows that expect one. The argument list is read with the
   compiler's own header, which names no type the kernel's disagree with. */
#include <stdarg.h>
static const char *last_klog;
static int last_link_mbps;
static const char *last_link_duplex;
static const char *last_link_pause;

static int has(const char *s, const char *sub);

/** Keep the format of a kernel log record, and the speed, duplex and pause of
 * a link line; discard the rest. */
void klog(int level, const char *subsystem, const char *format, ...)
{
    va_list ap;

    (void)level;
    (void)subsystem;
    last_klog = format;
    if (has(format, "link %d Mb/s")) {
        va_start(ap, format);
        last_link_mbps = va_arg(ap, int);
        last_link_duplex = va_arg(ap, const char *);
        last_link_pause = va_arg(ap, const char *);
        va_end(ap);
    }
}

/** Report whether `s` contains `sub`, and 0 for a NULL `s`; written out,
 * since this test pulls in no host header. */
static int has(const char *s, const char *sub)
{
    unsigned i, j;

    if (!s) {
        return 0;
    }
    for (i = 0; s[i]; i++) {
        for (j = 0; sub[j] && s[i + j] == sub[j]; j++) {
        }
        if (!sub[j]) {
            return 1;
        }
    }
    return 0;
}

/** Find the model's one RTL8168, at bus 1 on line 11, unless `pci_present`
 * is clear. */
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

/** Report whether `bar` is a memory BAR. */
int pci_bar_is_memory(unsigned int bar)
{
    return bar != 0 && (bar & 1u) == 0;
}

/** Report whether `bar` is a 64-bit memory BAR. */
int pci_bar_is_64bit(unsigned int bar)
{
    return (bar & 0x6u) == 0x4u;
}

/** Return the address of `bar`, without its flag bits. */
unsigned int pci_bar_address(unsigned int bar)
{
    return pci_bar_is_memory(bar) ? (bar & ~0xfu) : (bar & ~0x3u);
}

/** The whole address of memory BAR `i`, as sys/pci.c reads it. */
uint64_t pci_bar_address64(const pci_device_t *dev, int i)
{
    uint64_t phys;

    if (i < 0 || i > 5 || !pci_bar_is_memory(dev->bar[i])) {
        return 0;
    }
    phys = pci_bar_address(dev->bar[i]);
    if (pci_bar_is_64bit(dev->bar[i])) {
        if (i == 5) {
            return 0;
        }
        phys |= (uint64_t)dev->bar[i + 1] << 32;
    }
    return phys;
}

/* The card's messages: what the machine offers (an APIC, a vector), what the
 * driver registers, and what the device is told; a status the test holds
 * set is a storm. */
static int model_lapic = 1;
static int model_vector = 48;
static int model_msi_refuse; /* nonzero to make pci_msi_enable fail */
static int model_msi_on;
static int model_msi_vector = -1;
static isr_handler_t model_handler;
static int model_handler_vector = -1;
/* The handler for the vector was registered when the messages were enabled. */
static int model_msi_enabled_with_handler;
/* The write log's length when the messages were turned off, and on below. */
static unsigned model_msi_disable_at = 0xffffffffu;
static int model_sticky_status;
static unsigned model_acks;               /* writes to the status register */
static unsigned model_link_change_at_ack; /* the ack after which the link changes, 0 for none */
/* The chip's message is an edge: it is sent when the sources it may report go
 * from none set to one set. Unmasking with a source already set is that
 * transition, which is what a handler leaves behind for the next entry. */
static unsigned model_edges;            /* messages the chip would send */
static unsigned model_source_after_ack; /* a source set again as the ack lands */
static unsigned model_msi_enable_at = 0xffffffffu;
static int model_arrive_during_drain; /* frames that arrive while the ring is drained */
/* The receive descriptor the next arrival fills. */
static unsigned model_arrive_slot = 1;
static unsigned model_log_len;

/** Report whether the machine can deliver a message: `model_lapic`. */
int isr_msi_ready(void)
{
    return model_lapic;
}

/** Give `model_vector` as the vector allocated. */
int isr_alloc_vector(void)
{
    return model_vector;
}

/** Record the handler registered and its vector. */
void register_isr_handler(uint8_t vector, isr_handler_t handler, const char *name)
{
    (void)name;
    model_handler = handler;
    model_handler_vector = vector;
}

/**
 * Enable the card's messages on `vector`, or fail with -1 while
 * `model_msi_refuse` is set. Record the write log's length, and whether the
 * handler for the vector was registered first.
 */
int pci_msi_enable(const pci_device_t *dev, uint8_t vector)
{
    (void)dev;
    model_msi_enable_at = model_log_len;
    if (model_msi_refuse) {
        return -1;
    }
    model_msi_on = 1;
    model_msi_vector = vector;
    model_msi_enabled_with_handler = model_handler != 0 && model_handler_vector == (int)vector;
    return 0;
}

/** Turn the card's messages off, and record the write log's length. */
void pci_msi_disable(const pci_device_t *dev)
{
    (void)dev;
    model_msi_disable_at = model_log_len;
    model_msi_on = 0;
}

static int pci_master_enabled; /* the command word's bus-master bit */
static int pci_memory_enabled; /* the command word's memory-space bit */

/* The PCIe capability the link-state disable walks to, and the config-space
 * words it writes. The model presents a card whose link control has both ASPM
 * bits set, which is the state the target machine's previous operating system
 * leaves, and a bridge above it in the same state. */
#define MODEL_CAP_OFF 0x40
static unsigned short model_link_ctl_dev = 0x0003;
static unsigned short model_link_ctl_bridge = 0x0003;
static int model_bridge_present = 1;

/**
 * Read a configuration byte: the capability pointer, a PCI Express capability
 * with nothing after it, and the bridge's secondary bus, 1; any other byte
 * reads 0.
 */
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

/**
 * Read a configuration word: a status with a capability list, and the link
 * control of the card on bus 1 or of the bridge; any other word reads 0.
 */
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

/** Write the link control of the card on bus 1 or of the bridge; drop any
 * other write. */
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

/** Find the PCI Express capability at MODEL_CAP_OFF, and no other. */
unsigned char pci_find_capability(unsigned char bus, unsigned char device, unsigned char function,
                                  unsigned char cap_id)
{
    (void)bus;
    (void)device;
    (void)function;
    return cap_id == PCI_CAP_ID_PCIE ? MODEL_CAP_OFF : 0;
}

/** Give the bridge at 0:28.0 as the port above `bus`, unless
 * `model_bridge_present` is clear. */
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

/** Set the bus-master bit. */
void pci_enable_bus_master(pci_device_t *dev)
{
    (void)dev;
    pci_master_enabled = 1;
}

/** Set the memory-space bit. */
void pci_enable_memory_space(pci_device_t *dev)
{
    (void)dev;
    pci_memory_enabled = 1;
}

/** Clear the bus-master bit. */
void pci_disable_bus_master(pci_device_t *dev)
{
    (void)dev;
    pci_master_enabled = 0;
}

/** Map `size` bytes at `phys` one to one. */
uintptr_t map_mmio(uintptr_t phys, size_t size)
{
    (void)size;
    return phys;
}

/* The machine's station address, which net.h declares. */
unsigned char net_mac[6];

/* ------------------------------------------------------------------ */
/* The model of the chip.                                              */
/* ------------------------------------------------------------------ */

#define MODEL_REGS 256

static unsigned char model_reg[MODEL_REGS];

/* What the model does out of the ordinary, so a test can break one thing at a
 * time and watch the driver fail, not hang. */
static int model_stuck_eri;      /* the ERI completion flag never moves */
static int model_stuck_ocp;      /* the PHY-OCP completion flag never moves */
static int model_stuck_linklist; /* the link list never reports ready */
static int model_stuck_reset;    /* the reset bit never clears */
static int model_stuck_fifo;     /* the FIFOs never report empty */
/* Only the receive FIFO reports empty, the transmit one never does. A chip
   half way through draining is what a wait on "any bit of the mask" would
   accept and a wait on "all of it" rejects, which is the difference the
   reference's own condition makes. */
static int model_half_fifo;
static int model_swallow_doorbell; /* the doorbell does nothing */
/* The tally counter dump: the chip writes the block and clears the bit at
 * the first read after the request, or never when the test sticks it. The
 * values it writes are the model's own tallies, which a reset command zeroes
 * and a reset of the chip does not. */
static int model_stuck_counters;
static int model_counter_pending;   /* the command bit given, or zero */
static int model_counter_addressed; /* the low word was written as the address alone */
static unsigned model_counter_addr;
static unsigned model_counter_high_read; /* readbacks of the high word */
static unsigned
    model_tally[8]; /* tx errors, underruns, one, many, aborted, missed, align, multicast */
static unsigned udelay_calls;
static void model_counters_complete(void);
static unsigned model_xid_txconfig = 0x509u << 20; /* the revision reported */

/* The recorded write sequence. */
struct wr {
    unsigned char width;
    unsigned off;
    unsigned value;
};

#define MODEL_LOG_MAX 512
static struct wr model_log[MODEL_LOG_MAX];
static unsigned model_log_len; /* writes logged, MODEL_LOG_MAX and past it */

/** Log a write of `width` bits of `value` to `off`; the first MODEL_LOG_MAX
 * are kept. */
static void model_record(unsigned char width, unsigned off, unsigned value)
{
    if (model_log_len < MODEL_LOG_MAX) {
        model_log[model_log_len].width = width;
        model_log[model_log_len].off = off;
        model_log[model_log_len].value = value;
    }
    model_log_len++;
}

/** Read the model's 32-bit register at `off`, low byte first, with no side
 * effect. */
static unsigned model_r32_raw(unsigned off)
{
    return (unsigned)model_reg[off] | ((unsigned)model_reg[off + 1] << 8) |
           ((unsigned)model_reg[off + 2] << 16) | ((unsigned)model_reg[off + 3] << 24);
}

/** Store `v` in the model's 32-bit register at `off`, without logging it or
 * completing a command. */
static void model_w32_raw(unsigned off, unsigned v)
{
    model_reg[off] = (unsigned char)v;
    model_reg[off + 1] = (unsigned char)(v >> 8);
    model_reg[off + 2] = (unsigned char)(v >> 16);
    model_reg[off + 3] = (unsigned char)(v >> 24);
}

/**
 * Read the model's byte register at `off`. The command register reads with its
 * reset bit cleared unless `model_stuck_reset` holds it; MCU reads with the
 * FIFOs empty and the link list ready unless the model's faults say otherwise.
 */
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

/** Read the model's 16-bit register at `off`. */
static unsigned short m_r16(unsigned off)
{
    return (unsigned short)((unsigned)model_reg[off] | ((unsigned)model_reg[off + 1] << 8));
}

/**
 * Read the model's 32-bit register at `off`.
 *
 * A read of the counter address's low word completes a pending dump or tally
 * reset unless `model_stuck_counters` is set, and a read of the high word is
 * counted. TxConfig reads with the model's revision and, unless
 * `model_stuck_fifo` is set, its FIFO-empty bit. Every window register reads
 * back what its completed command left, since m_w32 is where a command
 * completes.
 */
static unsigned m_r32(unsigned off)
{
    if (off == R8168_COUNTERADDRHIGH) {
        model_counter_high_read++;
    }
    if (off == R8168_COUNTERADDRLOW && model_counter_pending && !model_stuck_counters) {
        if (model_counter_pending == R8168_COUNTER_DUMP) {
            model_counters_complete();
        } else {
            memset(model_tally, 0, sizeof model_tally);
        }
        model_w32_raw(off, model_r32_raw(off) & ~(unsigned)model_counter_pending);
        model_counter_pending = 0;
    }
    if (off == R8168_TXCONFIG) {
        unsigned v = model_r32_raw(off) & 0x000fffffu;

        v |= model_xid_txconfig;
        if (!model_stuck_fifo) {
            v |= R8168_TXCFG_EMPTY;
        }
        return v;
    }
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

/**
 * Write the model's byte register at `off`, and log the write.
 *
 * A write of the doorbell runs the chip's transmit engine, model_tx_drain,
 * which stands in for the DMA the driver waits on, unless
 * `model_swallow_doorbell` makes it a poll request the chip lost.
 */
static void m_w8(unsigned off, unsigned char v)
{
    model_record(8, off, v);
    if (off == R8168_TXPOLL) {
        if (model_swallow_doorbell) {
            return; /* a poll request the chip lost */
        }
        model_tx_drain();
        return;
    }
    model_reg[off] = v;
}

/**
 * Write the model's 16-bit register at `off`, and log the write.
 *
 * A status bit clears when it is written, unless `model_sticky_status` holds
 * the status set for a storm; the acknowledgement numbered
 * `model_link_change_at_ack` then sets the link source, and every one sets
 * `model_source_after_ack`, in a status the driver has just cleared. The chip's
 * interrupt output is the status masked by the mask register, and a message is
 * sent when that output goes from none set to one set, so a write of the mask
 * counts an edge only on that transition; counting a write instead would let a
 * handler that never masked anything pass.
 */
static void m_w16(unsigned off, unsigned short v)
{
    model_record(16, off, v);
    if (off == R8168_INTRSTATUS) {
        if (!model_sticky_status) {
            unsigned cur = m_r16(off) & ~(unsigned)v;

            model_acks++;
            if (model_link_change_at_ack != 0 && model_acks == model_link_change_at_ack) {
                cur |= R8168_INTR_LINKCHG;
            }
            cur |= model_source_after_ack;
            model_reg[off] = (unsigned char)cur;
            model_reg[off + 1] = (unsigned char)(cur >> 8);
        }
        return;
    }
    if (off == R8168_INTRMASK) {
        unsigned short before = m_r16(R8168_INTRSTATUS) & m_r16(R8168_INTRMASK);
        unsigned short after = m_r16(R8168_INTRSTATUS) & v;

        if (before == 0 && after != 0) {
            model_edges++;
        }
    }
    model_reg[off] = (unsigned char)v;
    model_reg[off + 1] = (unsigned char)(v >> 8);
}

/**
 * Write the model's 32-bit register at `off`, log the write, and complete a
 * command it gives.
 *
 * The chip dumps its tallies only when the command arrives after the address
 * was written on its own and the high word read back, which is the reference's
 * order; a command with the address in the same write, or one that never read
 * the high word, dumps to the old address in the reference's experience, and
 * here to nowhere.
 *
 * A window command completes here: a write by its flag going low, a read by the
 * flag going high with the data readable; a window the test has stuck never
 * reaches the state its caller polls for. An OCP command word has the register
 * in bits 30:16 as reg/2 (the driver forms it as reg << 15 and the register is
 * always even), so the data's top bit does not reach the address field.
 */
static void m_w32(unsigned off, unsigned v)
{
    model_record(32, off, v);
    model_w32_raw(off, v);

    if (off == R8168_COUNTERADDRLOW) {
        if (!(v & (R8168_COUNTER_DUMP | R8168_COUNTER_RESET))) {
            model_counter_addr = v;
            model_counter_addressed = 1;
        } else if (model_counter_addressed && model_counter_high_read &&
                   (v & ~0x3fu) == (model_counter_addr & ~0x3fu)) {
            model_counter_pending = (int)(v & (R8168_COUNTER_DUMP | R8168_COUNTER_RESET));
        }
    }

    if (off == R8168_OCPDR) {
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

/** Wait no time, and count the wait. */
static void m_udelay(unsigned usec)
{
    (void)usec;
    udelay_calls++;
}

/* ------------------------------------------------------------------ */

#include "network/driver/rtl8168.c"

/**
 * While `model_arrive_during_drain` counts frames, fill the next receive
 * descriptor with a 64-byte frame and raise the receive source again: a frame
 * that arrives while another is handed up.
 */
static void arrive_during_drain(void)
{
    if (model_arrive_during_drain > 0) {
        model_arrive_during_drain--;
        r8168_rx_ring[model_arrive_slot % R8168_RING_SIZE].opts1 =
            R8168_DESC_FS | R8168_DESC_LS | (64 + 4);
        model_arrive_slot++;
        model_reg[R8168_INTRSTATUS] =
            (unsigned char)(model_reg[R8168_INTRSTATUS] | R8168_INTR_RXOK);
    }
}

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

/* The model's register access, which model_reset installs as the driver's. */
static const struct r8168_io model_io = {m_r8, m_r16, m_r32, m_w8, m_w16, m_w32, m_udelay};

/** Store the 32-bit `v` at byte `off` of `block`, low byte first. */
static void model_put32(unsigned char *block, unsigned off, unsigned v)
{
    block[off] = (unsigned char)v;
    block[off + 1] = (unsigned char)(v >> 8);
    block[off + 2] = (unsigned char)(v >> 16);
    block[off + 3] = (unsigned char)(v >> 24);
}

/** Store the 16-bit `v` at byte `off` of `block`, low byte first. */
static void model_put16(unsigned char *block, unsigned off, unsigned v)
{
    block[off] = (unsigned char)v;
    block[off + 1] = (unsigned char)(v >> 8);
}

/**
 * Write the model's tallies into the tally block, as the chip does at the
 * address the request named, which here is the driver's own block since the
 * test includes the driver.
 *
 * The block's layout is the reference's, by byte offset, so a driver structure
 * that drifted from it would read the wrong fields.
 */
static void model_counters_complete(void)
{
    unsigned char *block = (unsigned char *)&r8168_counters;

    model_put32(block, 16, model_tally[0]); /* tx_errors, low word */
    model_put16(block, 62, model_tally[1]); /* tx_underrun */
    model_put32(block, 32, model_tally[2]); /* tx_one_collision */
    model_put32(block, 36, model_tally[3]); /* tx_multi_collision */
    model_put16(block, 60, model_tally[4]); /* tx_aborted */
    model_put16(block, 28, model_tally[5]); /* rx_missed */
    model_put16(block, 30, model_tally[6]); /* align_errors */
    model_put32(block, 56, model_tally[7]); /* rx_multicast */
}

/**
 * Send every transmit descriptor the chip owns and hand it back.
 *
 * The doorbell is what starts this on real hardware, so the model does it from
 * the doorbell write: without it the driver would find the ring full and wait,
 * bounded, for an ownership bit nothing clears, which is what the stall case
 * tests.
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

/**
 * Put the model and the driver back to a cold-power state, then seed the
 * registers as the chip is found on the laptop.
 *
 * The chip is not at its power-on state: another operating system ran before
 * this kernel, so every register the driver reads is seeded with a value that
 * is wrong for the driver, the out-of-band bit is set and CPlusCmd has every
 * bit set. A driver that skips a write, or keeps bits it should clear, fails a
 * check it would pass against registers of zeroes, and a wrong preserve mask
 * shows. The permanent station address the chip was built with is in the ERI
 * words at 0xe0, which is where the driver must take it from; the register pair
 * has a plausible but wrong address, so a driver that reads the pair is seen
 * to.
 */
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
    model_lapic = 1;
    model_vector = 48;
    model_msi_refuse = 0;
    model_msi_on = 0;
    model_msi_vector = -1;
    model_handler = 0;
    model_handler_vector = -1;
    model_msi_enabled_with_handler = 0;
    model_msi_disable_at = 0xffffffffu;
    model_sticky_status = 0;
    model_acks = 0;
    model_link_change_at_ack = 0;
    model_edges = 0;
    model_source_after_ack = 0;
    model_msi_enable_at = 0xffffffffu;
    model_arrive_during_drain = 0;
    model_arrive_slot = 1;
    rx_hook = 0;
    rx_first_at = 0;
    model_stuck_eri = 0;
    model_stuck_ocp = 0;
    model_stuck_linklist = 0;
    model_stuck_reset = 0;
    model_stuck_fifo = 0;
    model_half_fifo = 0;
    model_swallow_doorbell = 0;
    model_stuck_counters = 0;
    model_counter_pending = 0;
    model_counter_addressed = 0;
    model_counter_high_read = 0;
    model_counter_addr = 0;
    udelay_calls = 0;
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
    model_reg[R8168_CPLUSCMD] = 0xFF;
    model_reg[R8168_CPLUSCMD + 1] = 0xFF;
    model_eri[0xe0] = 0x7E44D228u;
    model_eri[0xe4] = 0x0000AFE0u;
    model_w32_raw(R8168_MAC0, 0xDEADBEEFu);
    model_w32_raw(R8168_MAC4, 0x0000BEEFu);
    model_link_ctl_dev = 0x0003;
    model_link_ctl_bridge = 0x0003;
    model_bridge_present = 1;
}

/** Return the index of the first logged write of `width` bits to `off`, at or
 * after `from`, or -1. */
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

/** Return the index of the first logged write of `width` bits of `value` to
 * `off`, at or after `from`, or -1. */
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

/** Return the index of the first logged ERI command, at or after `from`, that
 * writes `addr`, or -1. */
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

/**
 * Run the rows against the model, most of them after model_reset:
 *
 *   * the bring-up completes on a healthy model: decoding and bus mastering on,
 *     the link power states off at the card and at the port above it, the
 *     revision read from the chip, the station address from the permanent one
 *     rather than the register pair, and CPlusCmd keeping exactly the bits the
 *     reference preserves;
 *   * the identify and quiesce order: the out-of-band bit cleared, and the link
 *     list handshaked with one bit cleared and then another set, before any
 *     reset;
 *   * the receive gate closed before the reset and opened before the receiver
 *     is enabled;
 *   * the start sequence in the reference's order, searched from the last
 *     unlock, which is the start sequence's own: clock request, CPlusCmd and
 *     the early-transmit size; the chip-specific table, the EPHY table, the
 *     ring addresses high half first, and the lock; then the receive and
 *     transmit configurations, with the interrupt mask left at zero;
 *   * the exact values the reference writes, and the descriptor contract of the
 *     receive ring;
 *   * a different 8168 is rejected, an identifier that differs only in the bits
 *     the family mask ignores is the same chip and is accepted, a window that
 *     reads back all ones is an absent device and is rejected, and an absent
 *     card reports absence;
 *   * every handshake that never completes is bounded: the FIFOs, one FIFO
 *     still holding data, the link list, the reset, the ERI window and the PHY
 *     window;
 *   * transmit: the descriptor contract, and a short frame padded with zeroes.
 *     A chip that never answers the doorbell keeps every descriptor; the
 *     transmit is asynchronous, so the ring takes the first RING_SIZE frames,
 *     and the frame after them is a counted stall and not a wait without end,
 *     so the machine does not stop because the wire did. The tick rings the
 *     doorbell again while frames are outstanding, which recovers a poll
 *     request the chip lost;
 *   * a chip that answers takes three ring loads without a stall, and the tick
 *     reclaims every completed descriptor;
 *   * receive: the chip gives the length with the frame check sequence, which
 *     is stripped; an error descriptor is counted and not delivered; a frame
 *     split across descriptors and a length shorter than the checksum are
 *     dropped;
 *   * a full receive ring is drained in one pass and wraps back to its first
 *     descriptor, and both rings keep the end-of-ring mark on the last
 *     descriptor alone;
 *   * a driver that never came up neither sends nor receives;
 *   * the driver's table, and every network lock taken is released;
 *   * the card's messages: the vector the machine gave, the handler registered
 *     before the device is enabled, and the sources unmasked after it. A
 *     handler acknowledges the status before it drains the ring, and counts an
 *     entry with nothing to do as idle. A frame that arrives while the first is
 *     handed up is taken by the next round; frames that keep coming past the
 *     budget of both rounds are deferred to the tick and counted, which is not
 *     a storm;
 *   * the chip's message is an edge: it is sent when the masked sources go from
 *     none set to one set. A source set again as the acknowledgement lands
 *     stays set, and a handler that returned with the sources unmasked and one
 *     of them set would never be entered again, so every frame after it would
 *     wait for the tick. The handler masks the sources while it runs and
 *     unmasks them at its end, and that unmask is the edge the next frame is
 *     announced by; an unmask that changes nothing sends nothing;
 *   * a status that will not clear while frames arrive is not stuck, since the
 *     entry's first round did work. With nothing in the rings,
 *     R8168_STORM_ENTRIES such entries in a row mask the sources and count a
 *     storm, and the tick still drains the ring. Stop masks the sources before
 *     it turns the messages off. A card whose messages do not enable, or a
 *     machine with no usable APIC, is polled;
 *   * the link: the port advertises pause in both directions with its speeds,
 *     so a switch with a slower far port asks it to wait rather than drop a run
 *     of frames, and the advertisement is written before negotiation is
 *     restarted. A link change the card reports is logged with the speed,
 *     duplex and pause of one sample, and is not taken for a stuck source; a
 *     link that dropped is logged as down. The error sources are counted before
 *     the acknowledgement clears them, since a frame the chip could not send
 *     has no other record;
 *   * the storm rule against the driver's own frames: with a source the
 *     acknowledgement does not clear and nothing received, a frame of the
 *     driver's own that the chip returns meanwhile, the log's netconsole line
 *     among them, is no evidence that the source clears, so the entry counts as
 *     stuck. When a frame arrives during the first round, the second and last
 *     round acknowledges the receive source again, and the link changes after
 *     that acknowledgement, the acknowledgement after the rounds logs the
 *     change rather than clearing it unseen;
 *   * on a polled card the tick reads the status, logs a link change as the
 *     handler would, and acknowledges it;
 *   * the card's own counts: the bring-up zeroes a tally a warm restart kept; a
 *     dump writes the high word, the address alone and then the command, with
 *     the high word read back between, for the block at its own aligned
 *     address; the counts land in the columns the interface cannot keep, with
 *     no wait when the chip answers at once; a chip that never writes the block
 *     is reported as not answering after the bounded wait; and a stopped card
 *     is not asked.
 *
 * Returns the number of failed checks, at most 255.
 */
int main(void)
{
    int a, b, c, d, e;
    unsigned i;

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
    CHECK(model_iface.mac[0] == 0x28 && model_iface.mac[1] == 0xD2 && model_iface.mac[2] == 0x44 &&
              model_iface.mac[3] == 0x7E && model_iface.mac[4] == 0xE0 &&
              model_iface.mac[5] == 0xAF,
          "the station address came from the permanent one, not the register pair");
    CHECK((model_reg[R8168_CPLUSCMD] | (model_reg[R8168_CPLUSCMD + 1] << 8)) ==
              (0xFFFFu & R8168_CPCMD_MASK),
          "CPlusCmd kept exactly the bits the reference preserves");

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

    {
        int unlock = -1, cfg2 = -1, cpluscmd = -1, maxtx = -1, ephy_last = -1;
        int lock = -1, rxcfg = -1, txcfg = -1, intrmask = -1, ringlo = -1, ringhi = -1;

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

    CHECK((r8168_rx_ring[0].opts1 & R8168_DESC_OWN) != 0,
          "every receive descriptor is owned by the chip");
    CHECK((r8168_rx_ring[0].opts1 & R8168_DESC_LEN_MASK) == R8168_RX_BUF_SIZE,
          "a receive descriptor has the buffer size");
    CHECK((r8168_rx_ring[R8168_RING_SIZE - 1].opts1 & R8168_DESC_EOR) != 0,
          "the last receive descriptor ends the ring");
    CHECK((r8168_rx_ring[0].opts1 & R8168_DESC_EOR) == 0,
          "no other receive descriptor ends the ring");

    model_reset();
    model_xid_txconfig = 0x5c8u << 20; /* an 8168 that is not the GU */
    CHECK(rtl8168_init(&model_iface, 0) == NETDEV_FAILED, "a different 8168 is refused");
    CHECK(r8168_up == 0, "the driver stays down after a refusal");

    model_reset();
    model_xid_txconfig = (0x509u | 0x800u) << 20;
    CHECK(rtl8168_init(&model_iface, 0) == NETDEV_UP,
          "an identifier differing outside the mask is accepted");

    model_reset();
    model_xid_txconfig = 0xfff00000u;
    CHECK(rtl8168_init(&model_iface, 0) == NETDEV_FAILED, "an all-ones window is refused");

    model_reset();
    pci_present = 0;
    CHECK(rtl8168_init(&model_iface, 0) == NETDEV_ABSENT,
          "an absent card reports absence, not failure");

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
        for (f = 0; f < R8168_RING_SIZE; f++) {
            rtl8168_tx(model_iface.dev, frame, (unsigned short)sizeof(frame));
        }
        CHECK(r8168_tx_packets == R8168_RING_SIZE, "the ring absorbs a full burst");
        CHECK(r8168_tx_stalls == 0, "and nothing stalls while there is room");
        rtl8168_tx(model_iface.dev, frame, (unsigned short)sizeof(frame));
        CHECK(r8168_tx_stalls == 1, "the frame past a full ring is counted, not waited on");
        CHECK(r8168_tx_packets == R8168_RING_SIZE, "and is not counted as sent");

        {
            unsigned before = model_log_len;

            rtl8168_poll(model_iface.dev);
            CHECK(find_write(before, 8, R8168_TXPOLL) >= 0,
                  "the tick rings the doorbell again while frames are outstanding");
        }
    }

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

    model_reset();
    CHECK(rtl8168_init(&model_iface, 0) == NETDEV_UP,
          "the bring-up completes before the receive cases");
    {
        unsigned len = 64;

        for (i = 0; i < len; i++) {
            r8168_rx_buf[0][i] = (unsigned char)(i + 3);
        }
        r8168_rx_ring[0].opts1 = R8168_DESC_FS | R8168_DESC_LS | (len + 4);
        rtl8168_poll(model_iface.dev);
        CHECK(rx_calls == 1, "the frame was delivered");
        CHECK(rx_last_len == len, "the frame check sequence was stripped");
        CHECK(rx_last[0] == 3 && rx_last[63] == 66, "the payload was delivered");
        CHECK((r8168_rx_ring[0].opts1 & R8168_DESC_OWN) != 0,
              "the descriptor was returned to the chip");
        CHECK(r8168_rx_packets == 1, "the receive was counted");
    }

    {
        unsigned before = rx_calls;

        r8168_rx_ring[1].opts1 = R8168_DESC_FS | R8168_DESC_LS | R8168_DESC_RX_RES | 64;
        rtl8168_poll(model_iface.dev);
        CHECK(rx_calls == before, "an error frame is not delivered");
        CHECK(r8168_rx_errors == 1, "an error frame is counted");
    }

    {
        unsigned before = rx_calls;

        r8168_rx_ring[2].opts1 = R8168_DESC_FS | 64;
        rtl8168_poll(model_iface.dev);
        CHECK(rx_calls == before, "a fragment is not delivered");
        CHECK(r8168_rx_dropped == 1, "a fragment is counted as dropped");
    }

    {
        unsigned before = rx_calls;

        r8168_rx_ring[3].opts1 = R8168_DESC_FS | R8168_DESC_LS | 2;
        rtl8168_poll(model_iface.dev);
        CHECK(rx_calls == before, "a frame shorter than its checksum is not delivered");
        CHECK(r8168_rx_dropped == 2, "the short frame is counted as dropped");
    }

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

        rx_calls = 0;
        r8168_rx_ring[0].opts1 = R8168_DESC_FS | R8168_DESC_LS | 68;
        rtl8168_poll(model_iface.dev);
        CHECK(rx_calls == 1, "the ring wrapped back to its first descriptor");
    }

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

    model_reset();
    rx_calls = 0;
    CHECK(rtl8168_init(&model_iface, 0) == NETDEV_UP,
          "the bring-up completes with messages offered");
    CHECK(model_msi_vector == 48 && r8168_msi_vector == 48,
          "the card is told the vector the machine gave");
    CHECK(model_msi_enabled_with_handler,
          "the handler was registered before the device was enabled");
    CHECK(m_r16(R8168_INTRMASK) == R8168_INTR_ENABLED,
          "the sources are unmasked once the messages are on");
    CHECK(find_write_value(0, 16, R8168_INTRMASK, R8168_INTR_ENABLED) >= (int)model_msi_enable_at,
          "and not before the device was enabled");
    {
        unsigned len = 64;
        int ack_at;

        for (i = 0; i < len; i++) {
            r8168_rx_buf[0][i] = (unsigned char)(i + 3);
        }
        r8168_rx_ring[0].opts1 = R8168_DESC_FS | R8168_DESC_LS | (len + 4);
        model_reg[R8168_INTRSTATUS] = R8168_INTR_RXOK;
        {
            unsigned before = model_log_len;

            model_handler(0);
            ack_at = find_write(before, 16, R8168_INTRSTATUS);
        }
        CHECK(rx_calls == 1, "a message delivers the frame on the ring");
        CHECK(m_r16(R8168_INTRSTATUS) == 0, "and the status is acknowledged");
        CHECK(ack_at >= 0 && (unsigned)ack_at < rx_first_at,
              "the status was acknowledged before the ring was drained");
        CHECK(r8168_msi_entries == 1 && r8168_msi_idle == 0, "the message was counted as work");
        model_handler(0);
        CHECK(r8168_msi_entries == 2 && r8168_msi_idle == 1,
              "a message with nothing to do is counted as idle");

        rx_calls = 0;
        rx_hook = arrive_during_drain;
        r8168_rx_ring[1].opts1 = R8168_DESC_FS | R8168_DESC_LS | (len + 4);
        model_arrive_slot = 2;
        model_arrive_during_drain = 1;
        model_reg[R8168_INTRSTATUS] = R8168_INTR_RXOK;
        model_handler(0);
        CHECK(rx_calls == 2 && m_r16(R8168_INTRSTATUS) == 0 && r8168_msi_deferred == 0,
              "a frame arriving during the drain is taken by the next round");

        rx_calls = 0;
        r8168_rx_ring[3].opts1 = R8168_DESC_FS | R8168_DESC_LS | (len + 4);
        model_arrive_slot = 4;
        model_arrive_during_drain = 3 * R8168_RING_SIZE;
        model_reg[R8168_INTRSTATUS] = R8168_INTR_RXOK;
        model_handler(0);
        CHECK(r8168_msi_storms == 0 && r8168_msi_deferred == 1 && rx_calls == 2 * R8168_RING_SIZE &&
                  m_r16(R8168_INTRMASK) == R8168_INTR_ENABLED && m_r16(R8168_INTRSTATUS) == 0,
              "a bound reached with work still announced is deferred, not a storm");
        rx_calls = 0;
        rtl8168_poll(model_iface.dev);
        CHECK(rx_calls >= 1 && r8168_tick_work >= 1, "and the tick takes what was left, counted");
        model_arrive_during_drain = 0;

        rx_calls = 0;
        model_edges = 0;
        model_source_after_ack = R8168_INTR_RXOK;
        r8168_rx_ring[r8168_rx_cur % R8168_RING_SIZE].opts1 =
            R8168_DESC_FS | R8168_DESC_LS | (len + 4);
        model_reg[R8168_INTRSTATUS] = R8168_INTR_RXOK;
        model_handler(0);
        model_source_after_ack = 0;
        CHECK(rx_calls == 1 && (m_r16(R8168_INTRSTATUS) & R8168_INTR_RXOK) != 0,
              "a source set as the acknowledgement lands stands after the handler");
        CHECK(m_r16(R8168_INTRMASK) == R8168_INTR_ENABLED && model_edges == 1,
              "and the handler's unmask is the edge that arms the next message");
        model_edges = 0;
        m_w16(R8168_INTRMASK, R8168_INTR_ENABLED);
        CHECK(model_edges == 0, "an unmask that changes nothing is not an edge");
        model_reg[R8168_INTRSTATUS] = 0;
    }
    {
        unsigned before = model_log_len;

        model_sticky_status = 1;
        rx_calls = 0;
        r8168_rx_ring[r8168_rx_cur % R8168_RING_SIZE].opts1 =
            R8168_DESC_FS | R8168_DESC_LS | (64 + 4);
        model_reg[R8168_INTRSTATUS] = R8168_INTR_RXOK;
        model_handler(0);
        CHECK(rx_calls == 1 && r8168_msi_stuck == 0,
              "an entry that did work in its first round is not counted as stuck");

        model_reg[R8168_INTRSTATUS] = R8168_INTR_RXOK;
        for (i = 0; i < R8168_STORM_ENTRIES - 1; i++) {
            model_handler(0);
        }
        CHECK(r8168_msi_storms == 0, "entries whose status will not clear are counted first");
        model_handler(0);
        CHECK(r8168_msi_storms == 1, "a status that will not clear is a storm");
        CHECK(find_write_value(before, 16, R8168_INTRMASK, 0) >= 0 && m_r16(R8168_INTRMASK) == 0,
              "which leaves the sources masked");
        model_sticky_status = 0;
        model_reg[R8168_INTRSTATUS] = 0;
        rx_calls = 0;
        r8168_rx_ring[r8168_rx_cur % R8168_RING_SIZE].opts1 =
            R8168_DESC_FS | R8168_DESC_LS | (64 + 4);
        rtl8168_poll(model_iface.dev);
        CHECK(rx_calls == 1, "and the tick still drains the ring");
    }
    {
        unsigned before = model_log_len;
        int mask_at;

        rtl8168_stop(model_iface.dev);
        mask_at = find_write_value(before, 16, R8168_INTRMASK, 0);
        CHECK(!model_msi_on && r8168_msi_vector == -1, "the stop turns the messages off");
        CHECK(mask_at >= 0 && (unsigned)mask_at < model_msi_disable_at,
              "after the sources were masked");
    }
    model_reset();
    model_msi_refuse = 1;
    rx_calls = 0;
    CHECK(rtl8168_init(&model_iface, 0) == NETDEV_UP,
          "the bring-up completes when the card's messages do not enable");
    CHECK(r8168_msi_vector == -1 && m_r16(R8168_INTRMASK) == 0, "and the sources stay masked");
    r8168_rx_ring[0].opts1 = R8168_DESC_FS | R8168_DESC_LS | (64 + 4);
    rtl8168_poll(model_iface.dev);
    CHECK(rx_calls == 1, "and the tick's pump delivers");
    model_reset();
    model_lapic = 0;
    CHECK(rtl8168_init(&model_iface, 0) == NETDEV_UP && r8168_msi_vector == -1,
          "a machine with no usable APIC stays polled");

    model_reset();
    CHECK(rtl8168_init(&model_iface, 0) == NETDEV_UP, "the bring-up completes for the link rows");
    CHECK(model_phy[R8168_OCP_STD_PHY_BASE + 2 * R8168_MII_ADVERTISE] == 0x0de1,
          "the port advertises 10 and 100 in both duplexes with pause both ways");
    CHECK(model_phy[R8168_OCP_STD_PHY_BASE + 2 * R8168_MII_CTRL1000] == 0x0200,
          "and 1000 full duplex");
    {
        int adv = find_write_value(
            0, 32, R8168_GPHY_OCP,
            R8168_OCP_FLAG | ((R8168_OCP_STD_PHY_BASE + 2 * R8168_MII_ADVERTISE) << 15) | 0x0de1u);
        int restart = find_write_value(0, 32, R8168_GPHY_OCP,
                                       R8168_OCP_FLAG |
                                           ((R8168_OCP_STD_PHY_BASE + 2 * R8168_MII_BMCR) << 15) |
                                           (R8168_BMCR_ANENABLE | R8168_BMCR_ANRESTART));

        CHECK(adv >= 0 && restart >= 0 && adv < restart,
              "the advertisement is written before negotiation is restarted");
    }
    CHECK(has(r8168_pause_words(R8168_PHY_TXFLOW | R8168_PHY_RXFLOW), "both") &&
              has(r8168_pause_words(R8168_PHY_TXFLOW), "sent") &&
              has(r8168_pause_words(R8168_PHY_RXFLOW), "taken") &&
              has(r8168_pause_words(0), "none"),
          "the pause resolved is named in both directions, one, or none");
    if (model_handler) {
        model_reg[R8168_PHYSTATUS] = R8168_PHY_LINK | R8168_PHY_1000F | R8168_PHY_FULLDUP |
                                     R8168_PHY_TXFLOW | R8168_PHY_RXFLOW;
        model_reg[R8168_INTRSTATUS] = (unsigned char)R8168_INTR_LINKCHG;
        model_reg[R8168_INTRSTATUS + 1] = 0;
        last_klog = 0;
        r8168_msi_stuck = 0;
        r8168_msi_storms = 0;
        model_handler(0);
        CHECK(has(last_klog, "link %d Mb/s"), "a link change the card reports is logged");
        CHECK(last_link_mbps == 1000 && has(last_link_duplex, "full") &&
                  has(last_link_pause, "both"),
              "with the speed, the duplex and the pause of that one sample");
        CHECK(r8168_msi_stuck == 0 && r8168_msi_storms == 0, "and is not taken for a stuck source");
        model_reg[R8168_PHYSTATUS] = 0;
        model_reg[R8168_INTRSTATUS] = (unsigned char)R8168_INTR_LINKCHG;
        last_klog = 0;
        model_handler(0);
        CHECK(has(last_klog, "link down"), "a link that dropped is logged as down");

        r8168_txerr_reports = 0;
        r8168_rxerr_reports = 0;
        model_reg[R8168_INTRSTATUS] = (unsigned char)(R8168_INTR_TXERR | R8168_INTR_TXOK);
        model_handler(0);
        CHECK(r8168_txerr_reports == 1 && r8168_rxerr_reports == 0,
              "a message with the transmit error source is counted");
        model_reg[R8168_INTRSTATUS] = (unsigned char)R8168_INTR_RXERR;
        model_handler(0);
        CHECK(r8168_txerr_reports == 1 && r8168_rxerr_reports == 1,
              "and one with the receive error source");
    } else {
        CHECK(0, "the bring-up registered a handler for the link rows");
    }

    model_reset();
    CHECK(rtl8168_init(&model_iface, 0) == NETDEV_UP, "the bring-up completes for the storm rows");
    if (model_handler) {
        static const uint8_t frame[64] = {0};

        r8168_msi_stuck = 0;
        r8168_msi_storms = 0;
        model_swallow_doorbell = 1;
        CHECK(rtl8168_tx(model_iface.dev, frame, sizeof(frame)) == NETDEV_TX_OK,
              "a frame of the driver's own is outstanding");
        model_swallow_doorbell = 0;
        model_sticky_status = 1;
        model_reg[R8168_INTRSTATUS] = R8168_INTR_TXOK;
        model_reg[R8168_INTRSTATUS + 1] = 0;
        model_tx_drain(); /* the chip returns the frame as the handler runs */
        model_handler(0);
        CHECK(r8168_msi_stuck == 1,
              "a source that stays set is stuck though the driver's own frame came back");
        model_sticky_status = 0;
        model_reg[R8168_INTRSTATUS] = 0;
        r8168_msi_stuck = 0;

        model_reg[R8168_INTRSTATUS] = R8168_INTR_RXOK;
        model_reg[R8168_PHYSTATUS] = R8168_PHY_LINK | R8168_PHY_100M | R8168_PHY_FULLDUP;
        r8168_rx_ring[r8168_rx_cur % R8168_RING_SIZE].opts1 =
            R8168_DESC_FS | R8168_DESC_LS | (64 + 4);
        model_arrive_during_drain = 1;
        model_arrive_slot = r8168_rx_cur + 1;
        model_acks = 0;
        model_link_change_at_ack = R8168_ISR_ROUNDS;
        last_klog = 0;
        r8168_msi_deferred = 0;
        rx_hook = arrive_during_drain;
        model_handler(0);
        rx_hook = 0;
        model_arrive_during_drain = 0;
        model_link_change_at_ack = 0;
        CHECK(model_acks == R8168_ISR_ROUNDS + 1 && r8168_msi_deferred == 1 &&
                  has(last_klog, "link %d Mb/s") && last_link_mbps == 100,
              "a link change after the last round's acknowledgement is logged by the "
              "acknowledgement after the rounds");
    } else {
        CHECK(0, "the bring-up registered a handler for the storm rows");
    }

    model_reset();
    model_msi_refuse = 1;
    CHECK(rtl8168_init(&model_iface, 0) == NETDEV_UP, "the bring-up completes polled");
    model_reg[R8168_PHYSTATUS] = R8168_PHY_LINK | R8168_PHY_10M;
    model_reg[R8168_INTRSTATUS] = (unsigned char)R8168_INTR_LINKCHG;
    model_reg[R8168_INTRSTATUS + 1] = 0;
    last_klog = 0;
    rtl8168_poll(model_iface.dev);
    CHECK(has(last_klog, "link %d Mb/s") && last_link_mbps == 10 && has(last_link_duplex, "half"),
          "the tick logs a link change on a polled card, with the speed and the duplex");
    CHECK(m_r16(R8168_INTRSTATUS) == 0, "and acknowledges the status");

    model_reset();
    model_tally[0] = 34; /* errors of a boot before, which a warm restart keeps */
    CHECK(rtl8168_init(&model_iface, 0) == NETDEV_UP, "the bring-up completes for the counters");
    CHECK(model_tally[0] == 0 && find_write_value(0, 32, R8168_COUNTERADDRLOW,
                                                  model_counter_addr | R8168_COUNTER_RESET) >= 0,
          "the bring-up zeroes the tally a warm restart kept");
    {
        struct netdev_counters out;
        unsigned before = model_log_len;

        model_tally[0] = 5;
        model_tally[1] = 2;
        model_tally[2] = 3;
        model_tally[3] = 4;
        model_tally[4] = 6;
        model_tally[5] = 7;
        model_tally[6] = 8;
        model_tally[7] = 9;
        CHECK(rtl8168_netdev.counters == rtl8168_counters, "the table names the counters");
        udelay_calls = 0;
        CHECK(rtl8168_counters(model_iface.dev, &out) == 1, "the card answers with its counts");
        {
            int hi = find_write_value(before, 32, R8168_COUNTERADDRHIGH, 0);
            int lo = find_write_value(before, 32, R8168_COUNTERADDRLOW, model_counter_addr);
            int cmd = find_write_value(before, 32, R8168_COUNTERADDRLOW,
                                       model_counter_addr | R8168_COUNTER_DUMP);

            CHECK(hi >= 0 && lo > hi && cmd > lo,
                  "the chip was given the high word, the address alone, then the command, in "
                  "the reference's order");
            CHECK(model_counter_high_read >= 1, "with the high word read back between");
        }
        CHECK((model_counter_addr & 0x3fu) == 0 &&
                  (model_counter_addr & ~0x3fu) == ((unsigned)(uintptr_t)&r8168_counters & ~0x3fu),
              "for the block at its own address, aligned, the address written on its own");
        CHECK(model_r32_raw(R8168_COUNTERADDRHIGH) == 0,
              "and the high word of the address is zero");
        CHECK(out.tx_errors == 5 && out.tx_fifo == 2 && out.tx_collisions == 7 &&
                  out.tx_carrier == 6,
              "the transmit columns are the chip's errors, underruns, collisions of both kinds and "
              "aborts");
        CHECK(out.rx_missed == 7 && out.rx_frame == 8 && out.rx_multicast == 9,
              "the receive columns are the chip's missed frames, alignment errors and multicasts");
        CHECK(udelay_calls == 0, "with no wait when the chip answers at once");

        model_stuck_counters = 1;
        udelay_calls = 0;
        CHECK(rtl8168_counters(model_iface.dev, &out) == 0,
              "a chip that never writes the block is reported as not answering");
        CHECK(udelay_calls == R8168_COUNTERS_WAIT_STEPS, "after the bounded wait");
        model_stuck_counters = 0;

        rtl8168_stop(model_iface.dev);
        before = model_log_len;
        CHECK(rtl8168_counters(model_iface.dev, &out) == 0 &&
                  find_write(before, 32, R8168_COUNTERADDRLOW) < 0,
              "a stopped card is not asked");
    }

    return fails > 255 ? 255 : fails;
}
