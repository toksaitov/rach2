/*
 * rtl8139.c - Realtek RTL8139 PCI NIC driver.
 *
 * TX: four round-robin descriptors, each with its own buffer. Writing the
 * length to a TSD register starts the DMA. This driver does not wait for a
 * slot's previous transmission to complete before reusing it, because QEMU
 * finishes each one as the TSD is written; see the note on rtl8139_tx, which
 * is where that assumption is stated and where real hardware would differ.
 *
 * RX: one linear ring buffer. The chip prefixes each frame with a 4-byte header
 * (little-endian status word and length word). We validate that header before
 * trusting the length, and resync the receiver if it ever looks wrong. An
 * overflow drains the complete frames already in the ring.
 *
 * IRQ: registered at vector 32 + IRQ line. The shared IRQ router sends the
 * PIC EOI before calling us, so this driver must not touch the PIC. The card
 * is on a PCI line that can be shared (e.g. with the HDA device); see the
 * service-loop note in rtl8139_irq for the shared-line handling.
 */

#include "../../klog.h"
#include "rtl8139.h"
#include "../net.h"
#include "../ethernet/ethernet.h"
#include "../../kio.h"
#include "../../arch/x86/serial.h"
#include "../../arch/x86/isr.h"
#include "../../arch/x86/pci.h"
#include "../../lib/kstring.h"
#include "../../mm/heap.h"

/* I/O base port (BAR0, with the type bits stripped). */
static uint16_t iobase = 0;

/* PCI device record, kept so we can read the IRQ line. */
static pci_device_t rtl_pci;

/* Four TX buffers, 4-byte aligned as the card requires. */
static uint8_t tx_buf[RTL_TX_NUM][RTL_TX_BUF_SIZE] __attribute__((aligned(4)));
static int tx_idx = 0;

/* RX ring (see RTL_RX_BUF_SIZE), allocated page-aligned at init (out of BSS),
 * with the software read pointer. */
static uint8_t *rx_buf;
static uint32_t rx_ptr = 0;
static int rx_overflow_logged;

/* The most recent device discovery or initialization error. */
static const char *rtl_err = "no card";

static void rtl8139_irq(isr_params_t *params);
static void rtl8139_rx(void);
static void rtl8139_rx_reset(void);
static int rtl8139_link(void *dev, struct netdev_link *out);

/* The card's own station address, read from the chip. */
static uint8_t rtl8139_mac[6];

/* The interface this driver's one card is. Kept from the bring-up, so a
 * frame it delivers says where it arrived. */
static struct netdev *rtl8139_iface;

/**
 * Probe, reset, and bring up the RTL8139.
 *
 * Finds the card on the PCI bus, resets it, latches rtl8139_mac from the chip,
 * programs the RX ring and TX descriptor base addresses, sets the receive and
 * transmit configuration, registers the IRQ handler, and enables the receiver
 * and transmitter. The receiver and transmitter are enabled last, after the
 * buffers are in place, so no DMA can start against an unconfigured ring.
 *
 * A legacy line reaches the processor only through the 8259 pair, and the
 * vector is `32 + irq` in a byte, so a line above 15 is refused rather than
 * registered onto an exception vector. BAR0 must be an assigned I/O window:
 * an unassigned BAR reads zero and a memory BAR is not a port number, and
 * either would drive writes at unrelated legacy ports. Both are checked
 * before bus mastering is enabled, so a card refused for them never had the
 * bus.
 *
 * @return NETDEV_UP on success, NETDEV_ABSENT when absent, or NETDEV_FAILED
 *         with rtl8139_error() describing the failure.
 */
int rtl8139_init(struct netdev *nd, int nth)
{
    uint32_t mac_lo, mac_hi;
    uint32_t i;
    uint32_t timeout;

    nd->dev = 0; /* this driver keeps its one card in its own file */
    if (nth != 0) {
        return NETDEV_ABSENT;
    }
    rtl8139_iface = nd; /* only for the card this driver has */

    if (!pci_find_device_by_id(0x10EC, 0x8139, &rtl_pci)) {
        rtl_err = "RTL8139 not found on PCI bus";
        return NETDEV_ABSENT;
    }
    nd->pci_key = rtl_pci.bus << 8 | rtl_pci.device << 3 | rtl_pci.function;

    if (rtl_pci.irq > 15) {
        rtl_err = "RTL8139 has no usable IRQ line";
        return NETDEV_FAILED;
    }

    if ((rtl_pci.bar[0] & 1u) == 0 || pci_bar_address(rtl_pci.bar[0]) == 0) {
        rtl_err = "RTL8139 has no I/O window";
        return NETDEV_FAILED;
    }
    pci_enable_bus_master(&rtl_pci);
    iobase = (uint16_t)pci_bar_address(rtl_pci.bar[0]);

    /* Enter config-write mode so the configuration registers are writable. */
    outb(iobase + RTL_9346CR, 0xC0);

    /* Software reset; the RST bit self-clears when the reset completes. */
    outb(iobase + RTL_CHIPCMD, RTL_CMD_RST);
    timeout = 100000;
    while ((inb(iobase + RTL_CHIPCMD) & RTL_CMD_RST) && --timeout) {
        /* spin */
    }
    if (timeout == 0) {
        rtl_err = "RTL8139 soft reset timed out";
        return NETDEV_FAILED;
    }

    /* The MAC address is latched at I/O offsets 0x00-0x05 after reset. */
    mac_lo = inl(iobase + RTL_MAC0);
    mac_hi = inw(iobase + RTL_MAC4);
    rtl8139_mac[0] = (uint8_t)(mac_lo >> 0);
    rtl8139_mac[1] = (uint8_t)(mac_lo >> 8);
    rtl8139_mac[2] = (uint8_t)(mac_lo >> 16);
    rtl8139_mac[3] = (uint8_t)(mac_lo >> 24);
    rtl8139_mac[4] = (uint8_t)(mac_hi >> 0);
    rtl8139_mac[5] = (uint8_t)(mac_hi >> 8);

    /* Allocate the RX ring from kernel memory (page-aligned, which also
     * satisfies the chip's DMA alignment). Point the chip at it; Rach2 is
     * identity-mapped, so the pointer is also the physical (DMA) address. */
    if (!rx_buf) {
        rx_buf = mem_reserve_ptr(RTL_RX_BUF_SIZE, true);
        if (!rx_buf) {
            rtl_err = "RTL8139 cannot allocate RX ring";
            return NETDEV_FAILED;
        }
    }
    outl(iobase + RTL_RBSTART, (uint32_t)rx_buf);
    rx_ptr = 0;

    /* Pre-load each TX start address; the lengths are written per packet. */
    for (i = 0; i < RTL_TX_NUM; i++) {
        outl(iobase + RTL_TSAD0 + i * 4, (uint32_t)tx_buf[i]);
    }
    tx_idx = 0;

    outl(iobase + RTL_RCR, RTL_RCR_VALUE);
    outl(iobase + RTL_TCR, RTL_TCR_VALUE);

    /* Accept all multicast (fine on SLIRP). */
    outl(iobase + RTL_MAR0, 0xFFFFFFFF);
    outl(iobase + RTL_MAR4, 0xFFFFFFFF);

    /* Leave config-write mode. */
    outb(iobase + RTL_9346CR, 0x00);

    /* Clear any stale interrupt status, then unmask the events we handle. */
    outw(iobase + RTL_ISR, 0xFFFF);
    outw(iobase + RTL_IMR,
         RTL_ISR_ROK | RTL_ISR_TOK | RTL_ISR_RER | RTL_ISR_TER | RTL_ISR_RXOVW | RTL_ISR_FOVW);

    register_isr_handler((uint8_t)(32 + rtl_pci.irq), rtl8139_irq, "rtl8139");

    /* Enable the receiver and transmitter (after the buffers are
     * configured). */
    outb(iobase + RTL_CHIPCMD, RTL_CMD_RE | RTL_CMD_TE);

    /* The card's address is its interface's; netdev_probe sets net_mac. */
    for (i = 0; i < 6; i++) {
        nd->mac[i] = rtl8139_mac[i];
    }
    return NETDEV_UP;
}

const char *rtl8139_error(void)
{
    return rtl_err;
}

/**
 * Log this card's IRQ, I/O port and MAC, then its link.
 *
 * The kernel formatter has no field width, so the MAC is printed a nibble at
 * a time.
 */
static void rtl8139_print_info(void *dev)
{
    struct netdev_link link;

    (void)dev; /* this driver has one card */

    klog(KLOG_INFO, "rtl8139", "IRQ %d, I/O 0x%X, MAC %X%X:%X%X:%X%X:%X%X:%X%X:%X%X",
         (int)rtl_pci.irq, (unsigned)iobase, (unsigned)(rtl8139_mac[0] >> 4),
         (unsigned)(rtl8139_mac[0] & 0xF), (unsigned)(rtl8139_mac[1] >> 4),
         (unsigned)(rtl8139_mac[1] & 0xF), (unsigned)(rtl8139_mac[2] >> 4),
         (unsigned)(rtl8139_mac[2] & 0xF), (unsigned)(rtl8139_mac[3] >> 4),
         (unsigned)(rtl8139_mac[3] & 0xF), (unsigned)(rtl8139_mac[4] >> 4),
         (unsigned)(rtl8139_mac[4] & 0xF), (unsigned)(rtl8139_mac[5] >> 4),
         (unsigned)(rtl8139_mac[5] & 0xF));
    if (rtl8139_link(0, &link) && link.up) {
        klog(KLOG_INFO, "rtl8139", "link %d Mb/s, %s duplex", (int)link.mbps,
             link.full_duplex ? "full" : "half");
    } else {
        klog(KLOG_INFO, "rtl8139", "no link yet");
    }
}

/**
 * Silence the card: receiver and transmitter off, interrupts masked, DMA
 * revoked.
 *
 * There is nothing to drain first, unlike the other two: this driver does not
 * track transmit completion at all, because the machine it runs on finishes a
 * descriptor as the length is written (see the note on rtl8139_tx). A frame
 * handed to it has already gone.
 *
 * The receive ring is the card's to write into for as long as it is enabled
 * and mastering the bus, and that ring is kernel memory that outlives this
 * driver's interest in it. The order is enables, then mask, then bus
 * mastering: the card is told to stop while it still has a register interface
 * to be told through, and the ability to write memory is taken away last.
 *
 * Safe on a card that never finished starting, which is how the probe calls
 * it after a failed bring-up. The register writes are guarded on `iobase`,
 * which is zero until a usable port window was found, so nothing here writes
 * to port zero. Bus mastering is revoked outside that guard, whenever the
 * card was found.
 */
static void rtl8139_stop(void *dev)
{
    (void)dev; /* this driver has one card */
    if (iobase != 0) {
        outb(iobase + RTL_CHIPCMD, 0);
        outw(iobase + RTL_IMR, 0);
        outw(iobase + RTL_ISR, 0xFFFF);
    }
    if (rtl_pci.vendor_id == 0x10EC && rtl_pci.device_id == 0x8139) {
        pci_disable_bus_master(&rtl_pci);
    }
}

/**
 * Report the link, from the card's own media status register.
 *
 * `LINKB` is inverted, so the test reads the opposite way round from every
 * other bit here. Speed is one bit because this card has two speeds, and the
 * duplex comes from the basic mode control register, which is where the PHY
 * records what it negotiated.
 *
 * @return 1 with `*out` filled; 0 only when the driver has no port window,
 *         since a card that has one can always answer this.
 */
static int rtl8139_link(void *dev, struct netdev_link *out)
{
    uint8_t msr;

    (void)dev; /* this driver has one card */

    if (iobase == 0) {
        return 0;
    }
    msr = inb(iobase + RTL_MSR);
    out->up = (msr & RTL_MSR_LINKB) ? 0 : 1;
    out->mbps = (uint16_t)((msr & RTL_MSR_SPEED_10) ? 10 : 100);
    out->full_duplex = (inw(iobase + RTL_BMCR) & RTL_BMCR_DUPLEX) ? 1 : 0;
    return 1;
}

/* The card's entry in the driver table. `poll` is NULL: this one interrupts. */
const struct netdev_ops rtl8139_netdev = {
    "RTL8139",    rtl8139_init,  rtl8139_tx,         0, rtl8139_stop,
    rtl8139_link, rtl8139_error, rtl8139_print_info, 0};

/**
 * Copy a frame into the next TX descriptor and start its DMA.
 *
 * Frames larger than RTL_TX_BUF_SIZE are dropped; short frames are padded to
 * the 60-byte Ethernet minimum with zeroed pad so no stale buffer bytes leak
 * onto the wire. The descriptor is selected round-robin over the four slots.
 *
 * The four TX descriptors and tx_idx are shared between syscall-context and
 * IRQ-context senders, so the descriptor write runs with interrupts disabled
 * and the prior interrupt flag restored on return. In normal operation IF is
 * already 0 here (see the transmit concurrency note in net.h); the guard makes
 * that requirement explicit instead of trusting every caller to honor it.
 *
 * QEMU completes each transmit synchronously when the TSD is written, so a
 * descriptor is always free again by the time the four-slot rotation returns to
 * it; no OWN-bit poll is needed. Real hardware would poll the TSD OWN bit, but
 * this driver targets QEMU only.
 */
int rtl8139_tx(void *dev, const uint8_t *data, uint16_t len)
{
    uint16_t txlen;
    uint32_t eflags;

    (void)dev; /* this driver has one card */

    if (len > RTL_TX_BUF_SIZE) {
        klog(KLOG_WARN, "rtl8139", "tx: frame of %d bytes is too large, dropped", (int)len);
        return NETDEV_TX_DROPPED;
    }

    eflags = net_lock();

    k_memcpy(tx_buf[tx_idx], data, len);

    txlen = len;
    if (txlen < 60) {
        k_memset(tx_buf[tx_idx] + len, 0, (size_t)(60 - len));
        txlen = 60;
    }

    outl(iobase + RTL_TSAD0 + tx_idx * 4, (uint32_t)tx_buf[tx_idx]);
    outl(iobase + RTL_TSD0 + tx_idx * 4, (uint32_t)txlen & 0x1FFFUL);

    tx_idx = (tx_idx + 1) & 3;

    net_unlock(eflags);
    return NETDEV_TX_OK;
}

/**
 * Resync the receiver after a desynced ring header.
 *
 * Disables RX, resets the software read pointer, reprograms the ring (which
 * also resets the chip's internal write pointer), then re-enables RX.
 */
static void rtl8139_rx_reset(void)
{
    outb(iobase + RTL_CHIPCMD, RTL_CMD_TE);
    rx_ptr = 0;
    outl(iobase + RTL_RBSTART, (uint32_t)rx_buf);
    outl(iobase + RTL_RCR, RTL_RCR_VALUE);
    outw(iobase + RTL_CAPR, (uint16_t)(0 - 16));
    outw(iobase + RTL_ISR, RTL_ISR_RXOVW | RTL_ISR_FOVW | RTL_ISR_RER);
    outb(iobase + RTL_CHIPCMD, RTL_CMD_RE | RTL_CMD_TE);
}

/**
 * Drain completed frames from the RX ring up to the chip's write pointer as
 * it was when the call began.
 *
 * Reads each 4-byte ring header, validates its status and length before
 * trusting the length, hands the frame (minus its 4-byte CRC) to ethernet_rx,
 * and advances the read pointer and CAPR. A bad header means an error packet or
 * a desynced ring, so the receiver is resynced, not advanced by garbage.
 *
 * The drain stops at the write pointer it read on entry. Space goes back
 * through CAPR after every frame and the emulated card offers a queued packet
 * the moment it does, so a drain that ran to an empty ring could go on for as
 * long as frames keep arriving, with interrupts off, and starve the timer, the
 * scheduler and every other line. A frame past the mark raises the source
 * again and is taken by the next entry.
 */
static void rtl8139_rx(void)
{
    uint16_t status;
    uint16_t pkt_len;
    uint32_t advance;
    uint32_t bound;
    uint32_t taken;

    bound = (uint32_t)inw(iobase + RTL_CBR) % RTL_RX_RING;
    taken = 0;

    while (!(inb(iobase + RTL_CHIPCMD) & RTL_CMD_RXEMPTY)) {
        if (taken > 0 && rx_ptr == bound) {
            break;
        }
        taken++;
        /* The 4-byte ring header is little-endian: status then length. */
        status = (uint16_t)(rx_buf[rx_ptr] | (rx_buf[rx_ptr + 1] << 8));
        pkt_len = (uint16_t)(rx_buf[rx_ptr + 2] | (rx_buf[rx_ptr + 3] << 8));

        /* The length reads as a sentinel while the chip is still DMAing this
         * packet; stop and pick it up on the next interrupt. */
        if (pkt_len == RTL_RX_EARLY) {
            break;
        }

        if (!(status & RTL_ISR_ROK) || pkt_len < RTL_RX_PKT_MIN || pkt_len > RTL_RX_PKT_MAX) {
            klog(KLOG_WARN, "rtl8139", "rx: bad header status 0x%X len %d, resynchronising",
                 (unsigned)status, (int)pkt_len);
            ethernet_note_rx_error(rtl8139_iface);
            rtl8139_rx_reset();
            return;
        }

        /* Hand the frame up without its 4-byte CRC. */
        ethernet_rx(rtl8139_iface, rx_buf + rx_ptr + 4, (uint16_t)(pkt_len - 4));

        /* Advance past header + packet, rounded up to the next dword, and wrap
         * within the ring. CAPR trails the read pointer by 16 bytes. */
        advance = (uint32_t)pkt_len + 4 + 3;
        rx_ptr = (rx_ptr + advance) & ~3UL;
        rx_ptr %= RTL_RX_RING;
        outw(iobase + RTL_CAPR, (uint16_t)(rx_ptr - 16));
    }
}

/* Interrupt sources this driver acts on (TOK is unmasked but needs no work). */
#define RTL_ISR_HANDLED \
    (RTL_ISR_ROK | RTL_ISR_TOK | RTL_ISR_RER | RTL_ISR_TER | RTL_ISR_RXOVW | RTL_ISR_FOVW)

/**
 * Service the card until its interrupt status reads clear.
 *
 * The card shares a PCI interrupt line (e.g. with the HDA device), and a frame
 * finishing after ISR is acknowledged but before the ring drains would
 * re-assert ROK. Reading and acknowledging in a loop until nothing is pending
 * leaves the line deasserted and the ring drained on return, so no event is
 * stranded for the next IRQ. The guard bounds the loop should a status bit ever
 * stick. The shared IRQ router has already sent the PIC EOI, so this handler
 * must not touch the PIC.
 *
 * Overflow is counted on every entry but logged only once per boot, so
 * sustained traffic cannot evict the boot inventory with repeated warnings.
 * Draining the ring is the whole recovery: the card reports the overflow
 * instead of writing the packet that would not fit, so the frames already in
 * the ring are whole, and a reset would throw away frames that arrived since.
 * A ring that is actually desynced is caught by the header check in the
 * drain, which does reset.
 */
static void rtl8139_irq(isr_params_t *params)
{
    uint16_t isr_val;
    int guard;

    (void)params;

    for (guard = 0; guard < 64; guard++) {
        isr_val = inw(iobase + RTL_ISR);
        if ((isr_val & RTL_ISR_HANDLED) == 0) {
            break;
        }
        outw(iobase + RTL_ISR, isr_val); /* write-1-to-clear acknowledge */

        if (isr_val & RTL_ISR_ROK) {
            rtl8139_rx();
        }
        if (isr_val & RTL_ISR_RER) {
            klog(KLOG_WARN, "rtl8139", "rx: the card reported an error");
            ethernet_note_rx_error(rtl8139_iface);
        }
        if (isr_val & (RTL_ISR_RXOVW | RTL_ISR_FOVW)) {
            if (!rx_overflow_logged) {
                rx_overflow_logged = 1;
                klog(KLOG_WARN, "rtl8139",
                     "rx: the ring filled; draining it; further overflows are counted");
            }
            ethernet_note_rx_drop(rtl8139_iface);
            rtl8139_rx();
        }
        if (isr_val & RTL_ISR_TER) {
            klog(KLOG_WARN, "rtl8139", "tx: the card reported an error");
        }
        /* TOK needs no work here. This driver does not wait for a transmit
         * to complete at all; see the note on rtl8139_tx. */
    }
}
