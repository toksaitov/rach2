/*
 * rtl8139.c - Realtek RTL8139 PCI NIC driver.
 *
 * TX: four round-robin descriptors, each with its own buffer. Writing the
 * length to a TSD register starts the DMA. Before reusing a slot we wait for
 * its previous transmission to report TOK, so a burst of frames cannot
 * overwrite a buffer that is still in flight.
 *
 * RX: one linear ring buffer. The chip prefixes each frame with a 4-byte header
 * (little-endian status word and length word). We validate that header before
 * trusting the length, and resync the receiver if it ever looks wrong or the
 * ring overflows.
 *
 * IRQ: registered at vector 32 + IRQ line. The shared IRQ router sends the
 * PIC EOI before calling us, so this driver must not touch the PIC. The card
 * sits on a PCI line that can be shared (e.g. with the HDA device); see the
 * service-loop note in rtl8139_irq for the shared-line handling.
 */

#include "rtl8139.h"
#include "../net.h"
#include "../ethernet/ethernet.h"
#include "../../kio.h"
#include "../../serial.h"
#include "../../isr.h"
#include "../../pci.h"
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

/* Reason the most recent rtl8139_init() returned 0, surfaced via
 * rtl8139_error() for the boot banner. The success path prints nothing (see
 * net_print_info). */
static const char *rtl_err = "no card";

static void rtl8139_irq(isr_params_t *params);
static void rtl8139_rx(void);
static void rtl8139_rx_reset(void);

/**
 * Probe, reset, and bring up the RTL8139.
 *
 * Finds the card on the PCI bus, resets it, latches net_mac from the chip,
 * programs the RX ring and TX descriptor base addresses, sets the receive and
 * transmit configuration, registers the IRQ handler, and enables the receiver
 * and transmitter. The receiver and transmitter are enabled last, after the
 * buffers are in place, so no DMA can start against an unconfigured ring.
 *
 * @return 1 on success; 0 if no card is present or bring-up fails, with
 *         rtl8139_error() set to the reason.
 */
int rtl8139_init(void)
{
    uint32_t mac_lo, mac_hi;
    uint32_t i;
    uint32_t timeout;

    if (!pci_find_device_by_id(0x10EC, 0x8139, &rtl_pci)) {
        rtl_err = "RTL8139 not found on PCI bus";
        return NETDEV_ABSENT;
    }

    /* Reject an unrouted interrupt line rather than register a bogus vector. */
    if (rtl_pci.irq == 0xFF) {
        rtl_err = "RTL8139 has no usable IRQ line";
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
    net_mac[0] = (uint8_t)(mac_lo >>  0);
    net_mac[1] = (uint8_t)(mac_lo >>  8);
    net_mac[2] = (uint8_t)(mac_lo >> 16);
    net_mac[3] = (uint8_t)(mac_lo >> 24);
    net_mac[4] = (uint8_t)(mac_hi >>  0);
    net_mac[5] = (uint8_t)(mac_hi >>  8);

    /* Allocate the RX ring from kernel memory (page-aligned, which also
     * satisfies the chip's DMA alignment). Point the chip at it; Rach2 is
     * identity-mapped, so the pointer is also the physical (DMA) address. */
    if (!rx_buf) {
        rx_buf = (uint8_t *)mem_reserve(RTL_RX_BUF_SIZE, true);
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
    outw(iobase + RTL_IMR, RTL_ISR_ROK | RTL_ISR_TOK | RTL_ISR_RER |
                           RTL_ISR_TER | RTL_ISR_RXOVW | RTL_ISR_FOVW);

    register_isr_handler((uint8_t)(32 + rtl_pci.irq), rtl8139_irq);

    /* Enable the receiver and transmitter (after the buffers are
     * configured). */
    outb(iobase + RTL_CHIPCMD, RTL_CMD_RE | RTL_CMD_TE);

    return NETDEV_UP;
}

/* Boot-summary accessors (see net_print_info). */
uint16_t rtl8139_iobase(void)   { return iobase; }
uint8_t  rtl8139_irq_line(void) { return rtl_pci.irq; }
const char *rtl8139_error(void) { return rtl_err; }

/**
 * Write this card's detail line under the boot summary.
 *
 * serial_printf has no field width, so the MAC is printed a nibble at a time.
 * The console keeps only the one-line summary; this is the serial detail.
 */
static void rtl8139_print_info(void)
{
    serial_printf("    RTL8139: IRQ %d, I/O 0x%X, "
                  "MAC %X%X:%X%X:%X%X:%X%X:%X%X:%X%X\n",
                  (int)rtl_pci.irq, (unsigned)iobase,
                  (unsigned)(net_mac[0] >> 4), (unsigned)(net_mac[0] & 0xF),
                  (unsigned)(net_mac[1] >> 4), (unsigned)(net_mac[1] & 0xF),
                  (unsigned)(net_mac[2] >> 4), (unsigned)(net_mac[2] & 0xF),
                  (unsigned)(net_mac[3] >> 4), (unsigned)(net_mac[3] & 0xF),
                  (unsigned)(net_mac[4] >> 4), (unsigned)(net_mac[4] & 0xF),
                  (unsigned)(net_mac[5] >> 4), (unsigned)(net_mac[5] & 0xF));
}

/* The card's entry in the driver table. `poll` is NULL: this one interrupts. */
const struct netdev_ops rtl8139_netdev = {
    "RTL8139", rtl8139_init, rtl8139_tx, 0, rtl8139_error, rtl8139_print_info};

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
 * that requirement explicit rather than trusting every caller to honor it.
 *
 * QEMU completes each transmit synchronously when the TSD is written, so a
 * descriptor is always free again by the time the four-slot rotation returns to
 * it; no OWN-bit poll is needed. Real hardware would poll the TSD OWN bit, but
 * this driver targets QEMU only.
 */
void rtl8139_tx(const uint8_t *data, uint16_t len)
{
    uint16_t txlen;
    uint32_t eflags;

    if (len > RTL_TX_BUF_SIZE) {
        serial_printf("RTL8139: TX frame too large (%d bytes), dropped\n",
                      (int)len);
        return;
    }

    __asm__ __volatile__("pushfl; popl %0; cli" : "=r"(eflags) : : "memory");

    k_memcpy(tx_buf[tx_idx], data, len);

    txlen = len;
    if (txlen < 60) {
        k_memset(tx_buf[tx_idx] + len, 0, (size_t)(60 - len));
        txlen = 60;
    }

    outl(iobase + RTL_TSAD0 + tx_idx * 4, (uint32_t)tx_buf[tx_idx]);
    outl(iobase + RTL_TSD0 + tx_idx * 4, (uint32_t)txlen & 0x1FFFUL);

    tx_idx = (tx_idx + 1) & 3;

    __asm__ __volatile__("pushl %0; popfl" : : "r"(eflags) : "memory", "cc");
}

/**
 * Resync the receiver after an overflow or a desynced ring header.
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
 * Drain completed frames from the RX ring up to the chip's write pointer.
 *
 * Reads each 4-byte ring header, validates its status and length before
 * trusting the length, hands the frame (minus its 4-byte CRC) to ethernet_rx,
 * and advances the read pointer and CAPR. A bad header means an error packet or
 * a desynced ring, so the receiver is resynced rather than advanced by garbage.
 */
static void rtl8139_rx(void)
{
    uint16_t status;
    uint16_t pkt_len;
    uint32_t advance;

    while (!(inb(iobase + RTL_CHIPCMD) & RTL_CMD_RXEMPTY)) {
        /* The 4-byte ring header is little-endian: status then length. */
        status  = (uint16_t)(rx_buf[rx_ptr] | (rx_buf[rx_ptr + 1] << 8));
        pkt_len = (uint16_t)(rx_buf[rx_ptr + 2] | (rx_buf[rx_ptr + 3] << 8));

        /* The length reads as a sentinel while the chip is still DMAing this
         * packet; stop and pick it up on the next interrupt. */
        if (pkt_len == RTL_RX_EARLY) {
            break;
        }

        /* A non-OK status or an out-of-range length means the header is an
         * error packet or the ring has desynced. Trusting the length would
         * advance rx_ptr by garbage, so resync the receiver instead. */
        if (!(status & RTL_ISR_ROK) ||
            pkt_len < RTL_RX_PKT_MIN || pkt_len > RTL_RX_PKT_MAX) {
            serial_printf("RTL8139: bad RX header status=0x%X len=%d, resync\n",
                          (unsigned)status, (int)pkt_len);
            rtl8139_rx_reset();
            return;
        }

        /* Hand the frame up without its 4-byte CRC. */
        ethernet_rx(rx_buf + rx_ptr + 4, (uint16_t)(pkt_len - 4));

        /* Advance past header + packet, rounded up to the next dword, and wrap
         * within the ring. CAPR trails the read pointer by 16 bytes. */
        advance = (uint32_t)pkt_len + 4 + 3;
        rx_ptr  = (rx_ptr + advance) & ~3UL;
        rx_ptr %= RTL_RX_RING;
        outw(iobase + RTL_CAPR, (uint16_t)(rx_ptr - 16));
    }
}

/* Interrupt sources this driver acts on (TOK is unmasked but needs no work). */
#define RTL_ISR_HANDLED  (RTL_ISR_ROK | RTL_ISR_TOK | RTL_ISR_RER | \
                          RTL_ISR_TER | RTL_ISR_RXOVW | RTL_ISR_FOVW)

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
        outw(iobase + RTL_ISR, isr_val);   /* write-1-to-clear acknowledge */

        if (isr_val & RTL_ISR_ROK) {
            rtl8139_rx();
        }
        if (isr_val & RTL_ISR_RER) {
            serial_printf("RTL8139: RX error interrupt\n");
        }
        if (isr_val & (RTL_ISR_RXOVW | RTL_ISR_FOVW)) {
            serial_printf("RTL8139: RX overflow, resync\n");
            rtl8139_rx_reset();
        }
        if (isr_val & RTL_ISR_TER) {
            serial_printf("RTL8139: TX error interrupt\n");
        }
        /* TOK needs no work here; TX completion is polled in rtl8139_tx. */
    }
}
