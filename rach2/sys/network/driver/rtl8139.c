/*
 * rtl8139.c: the driver of one Realtek RTL8139, the card QEMU gets from
 * rach2/qemu_devices.sh by default.
 *
 * Transmit uses the card's four descriptors in turn, each with a buffer of
 * its own; writing a frame's length to a descriptor's TSD register starts
 * it. A slot is used again without a check that its last frame has gone,
 * since QEMU's card finishes a frame as its length is written; a physical
 * card would need the TSD's OWN bit checked first.
 *
 * Receive is one ring buffer, in which the card puts each frame after a
 * 4-byte header of a status word and a length. The header is checked before
 * the length is used, and a header that cannot be right restarts the
 * receiver. An overflow is recovered by taking the whole frames already in
 * the ring.
 *
 * The handler is registered at vector `32 + line`, and the line may be
 * shared, with the HDA controller among others, so the handler services the
 * card until its status reads clear. The driver has one card and keeps it in
 * this file's statics: the `dev` every operation is given is NULL and
 * ignored.
 */

#include "../../klog.h"
#include "rtl8139.h"
#include "../net.h"
#include "../ethernet/ethernet.h"
#include "../../kio.h"
#include "../../serial.h"
#include "../../irq.h"
#include "../../pci.h"
#include "../../lib/kstring.h"
#include "../../mm/heap.h"

/* The I/O window's first port, BAR0 without its type bit; 0 until a usable
 * window was found. */
static uint16_t iobase = 0;

/* The card's PCI function, with its interrupt line. */
static pci_device_t rtl_pci;

/* The four transmit buffers, 4-byte aligned as TSAD requires, and the slot
 * the next frame goes in. */
static uint8_t tx_buf[RTL_TX_NUM][RTL_TX_BUF_SIZE] __attribute__((aligned(4)));
static int tx_idx = 0;

/* The receive buffer (RTL_RX_BUF_SIZE bytes), taken from the boot
 * allocator at the first bring-up and kept; the offset in the ring of the
 * next header to read; and whether the one overflow warning of the boot was
 * logged. */
static uint8_t *rx_buf;
static uint32_t rx_ptr = 0;
static int rx_overflow_logged;

/* The reason the last bring-up that found no card or failed gave. */
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
 * The card is reset with its configuration registers unlocked, its station
 * address read, the receive buffer and the transmit slots' addresses given
 * to it, its configuration and filters set, and its interrupts unmasked; the
 * receiver and transmitter are enabled last, once the buffers are in place,
 * so that no DMA starts on a ring that is not set up. The receive buffer is
 * from the boot allocator, page-aligned and below the window's top, so its
 * physical address fits the 32 bits of RBSTART; it is kept for a later
 * bring-up.
 *
 * The platform's lines are 0 to 15, at vectors `32 + irq`; a line above 15
 * has no such vector, and 255, a function's line when it has none, would
 * wrap the byte onto an exception vector, so such a line is refused. BAR0
 * must be an assigned I/O window: an unassigned BAR reads zero and a memory
 * BAR is not a port number, and either would send writes to unrelated
 * ports. Both are checked before bus mastering is enabled, so a card refused
 * for them never had the bus. The reset's wait is a count of reads, not a
 * time.
 */
int rtl8139_init(struct netdev *nd, int nth)
{
    uint32_t mac_lo, mac_hi;
    uint32_t i;
    uint32_t timeout;

    nd->dev = 0;
    if (nth != 0) {
        return NETDEV_ABSENT;
    }
    rtl8139_iface = nd;

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

    /* 0xC0 in 9346CR unlocks the configuration registers. */
    pci_io_write8(iobase + RTL_9346CR, 0xC0);

    /* RST clears itself when the reset is done. */
    pci_io_write8(iobase + RTL_CHIPCMD, RTL_CMD_RST);
    timeout = 100000;
    while ((pci_io_read8(iobase + RTL_CHIPCMD) & RTL_CMD_RST) && --timeout) {
        /* spin */
    }
    if (timeout == 0) {
        rtl_err = "RTL8139 soft reset timed out";
        return NETDEV_FAILED;
    }

    /* IDR0 to IDR5, ports 0x00 to 0x05, have the station address. */
    mac_lo = pci_io_read32(iobase + RTL_MAC0);
    mac_hi = pci_io_read16(iobase + RTL_MAC4);
    rtl8139_mac[0] = (uint8_t)(mac_lo >> 0);
    rtl8139_mac[1] = (uint8_t)(mac_lo >> 8);
    rtl8139_mac[2] = (uint8_t)(mac_lo >> 16);
    rtl8139_mac[3] = (uint8_t)(mac_lo >> 24);
    rtl8139_mac[4] = (uint8_t)(mac_hi >> 0);
    rtl8139_mac[5] = (uint8_t)(mac_hi >> 8);

    if (!rx_buf) {
        rx_buf = mem_reserve_ptr(RTL_RX_BUF_SIZE, true);
        if (!rx_buf) {
            rtl_err = "RTL8139 cannot allocate RX ring";
            return NETDEV_FAILED;
        }
    }
    pci_io_write32(iobase + RTL_RBSTART, (uint32_t)virt_to_phys(rx_buf));
    rx_ptr = 0;

    for (i = 0; i < RTL_TX_NUM; i++) {
        pci_io_write32(iobase + RTL_TSAD0 + i * 4, (uint32_t)virt_to_phys(tx_buf[i]));
    }
    tx_idx = 0;

    pci_io_write32(iobase + RTL_RCR, RTL_RCR_VALUE);
    pci_io_write32(iobase + RTL_TCR, RTL_TCR_VALUE);

    /* A multicast filter of all ones passes every group. */
    pci_io_write32(iobase + RTL_MAR0, 0xFFFFFFFF);
    pci_io_write32(iobase + RTL_MAR4, 0xFFFFFFFF);

    /* 0x00 in 9346CR locks the configuration registers again. */
    pci_io_write8(iobase + RTL_9346CR, 0x00);

    /* ISR bits are cleared by writing them as 1. */
    pci_io_write16(iobase + RTL_ISR, 0xFFFF);
    pci_io_write16(iobase + RTL_IMR, RTL_ISR_ROK | RTL_ISR_TOK | RTL_ISR_RER | RTL_ISR_TER |
                                         RTL_ISR_RXOVW | RTL_ISR_FOVW);

    register_isr_handler((uint8_t)IRQ_VECTOR(rtl_pci.irq), rtl8139_irq, "rtl8139");

    pci_io_write8(iobase + RTL_CHIPCMD, RTL_CMD_RE | RTL_CMD_TE);

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
 * The driver table's `print_info`: log the card's interrupt line, I/O port
 * and station address, then its link.
 *
 * The kernel formatter has no field width, so the address is printed a
 * nibble at a time.
 */
static void rtl8139_print_info(void *dev)
{
    struct netdev_link link;

    (void)dev;

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
 * The driver table's `stop`: receiver and transmitter off, interrupts masked
 * and acknowledged, and bus mastering cleared, which ends the card's DMA.
 *
 * Unlike the other two drivers' `stop`, this one waits for no frame to be
 * sent: the driver does not follow transmit completion, and QEMU's card has
 * sent a frame by the time `rtl8139_tx` returns.
 *
 * The card may write the receive ring for as long as it is enabled and a bus
 * master, and the ring is kernel memory. The order is the enables, then the
 * mask, then bus mastering: the card is told to stop through its registers,
 * and its access to memory is taken away last.
 *
 * Safe on a card that never finished starting, which is how the probe calls
 * it after a failed bring-up. The register writes depend on `iobase`, which
 * is zero until a usable port window was found, so nothing here writes to
 * port zero. Bus mastering is cleared whenever the card was found.
 */
static void rtl8139_stop(void *dev)
{
    (void)dev;
    if (iobase != 0) {
        pci_io_write8(iobase + RTL_CHIPCMD, 0);
        pci_io_write16(iobase + RTL_IMR, 0);
        pci_io_write16(iobase + RTL_ISR, 0xFFFF);
    }
    if (rtl_pci.vendor_id == 0x10EC && rtl_pci.device_id == 0x8139) {
        pci_disable_bus_master(&rtl_pci);
    }
}

/**
 * The driver table's `link`, from the card's media status register.
 *
 * `LINKB` is inverted, so its test reads the opposite way round from every
 * other bit here. The speed is one bit because this card has two speeds, and
 * the duplex comes from the basic mode control register, where the PHY
 * records what it negotiated.
 *
 * @return 1 with `*out` filled; 0 only when the driver has no port window,
 *         since a card that has one can always answer this.
 */
static int rtl8139_link(void *dev, struct netdev_link *out)
{
    uint8_t msr;

    (void)dev;

    if (iobase == 0) {
        return 0;
    }
    msr = pci_io_read8(iobase + RTL_MSR);
    out->up = (msr & RTL_MSR_LINKB) ? 0 : 1;
    out->mbps = (uint16_t)((msr & RTL_MSR_SPEED_10) ? 10 : 100);
    out->full_duplex = (pci_io_read16(iobase + RTL_BMCR) & RTL_BMCR_DUPLEX) ? 1 : 0;
    return 1;
}

/* The card's entry in the driver table. `poll` is NULL: this one interrupts. */
const struct netdev_ops rtl8139_netdev = {
    "RTL8139",    rtl8139_init,  rtl8139_tx,         0, rtl8139_stop,
    rtl8139_link, rtl8139_error, rtl8139_print_info, 0};

/**
 * A short frame is padded with zeroes to the 60-byte minimum, so that no
 * bytes of an earlier frame are sent. The four slots and `tx_idx` are shared
 * by senders in system calls and in interrupt handlers, so the copy and the
 * register writes run under `net_lock`, which nests.
 *
 * QEMU's card sends a frame as its TSD is written, so a slot is free again
 * by the time the rotation comes back to it, and the OWN bit is not polled.
 */
int rtl8139_tx(void *dev, const uint8_t *data, uint16_t len)
{
    uint16_t txlen;
    uint32_t eflags;

    (void)dev;

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

    pci_io_write32(iobase + RTL_TSAD0 + tx_idx * 4, (uint32_t)virt_to_phys(tx_buf[tx_idx]));
    pci_io_write32(iobase + RTL_TSD0 + tx_idx * 4, (uint32_t)txlen & 0x1FFFUL);

    tx_idx = (tx_idx + 1) & 3;

    net_unlock(eflags);
    return NETDEV_TX_OK;
}

/**
 * Restart the receiver at the start of the ring, after a ring header that
 * cannot be right. The frames in the ring are lost.
 *
 * With the receiver off, the ring's address and the receive configuration
 * are written again, which also returns the card's write offset to the
 * start; this driver's read offset goes to 0 and CAPR to 16 bytes before
 * it, and the error and overflow bits of the status are acknowledged.
 */
static void rtl8139_rx_reset(void)
{
    pci_io_write8(iobase + RTL_CHIPCMD, RTL_CMD_TE);
    rx_ptr = 0;
    pci_io_write32(iobase + RTL_RBSTART, (uint32_t)virt_to_phys(rx_buf));
    pci_io_write32(iobase + RTL_RCR, RTL_RCR_VALUE);
    pci_io_write16(iobase + RTL_CAPR, (uint16_t)(0 - 16));
    pci_io_write16(iobase + RTL_ISR, RTL_ISR_RXOVW | RTL_ISR_FOVW | RTL_ISR_RER);
    pci_io_write8(iobase + RTL_CHIPCMD, RTL_CMD_RE | RTL_CMD_TE);
}

/**
 * Give the frames in the receive ring to `ethernet_rx`, up to the card's
 * write offset as it was when the call began. Called from the interrupt
 * handler, with interrupts off.
 *
 * Each header's status and length are checked before the length is used,
 * and each frame is given without its 4-byte frame check sequence; the read
 * offset and CAPR then move past it. A bad header is an errored frame or a
 * ring this driver no longer reads in step with, so it is counted as a
 * receive error and the receiver restarted (`rtl8139_rx_reset`). A length
 * of RTL_RX_EARLY is a frame the card is still writing, which the next
 * interrupt takes.
 *
 * The drain stops at the write offset it read on entry. Space goes back
 * through CAPR after every frame and QEMU's card gives a queued frame at
 * once, so a drain that ran to an empty ring could go on for as long as
 * frames keep arriving, with interrupts off, and starve the timer, the
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

    bound = (uint32_t)pci_io_read16(iobase + RTL_CBR) % RTL_RX_RING;
    taken = 0;

    while (!(pci_io_read8(iobase + RTL_CHIPCMD) & RTL_CMD_RXEMPTY)) {
        if (taken > 0 && rx_ptr == bound) {
            break;
        }
        taken++;
        /* The 4-byte ring header is little-endian: status then length. */
        status = (uint16_t)(rx_buf[rx_ptr] | (rx_buf[rx_ptr + 1] << 8));
        pkt_len = (uint16_t)(rx_buf[rx_ptr + 2] | (rx_buf[rx_ptr + 3] << 8));

        /* The length is RTL_RX_EARLY while the card is still writing. */
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

        /* The length counts the 4-byte frame check sequence. */
        ethernet_rx(rtl8139_iface, rx_buf + rx_ptr + 4, (uint16_t)(pkt_len - 4));

        /* Headers are 4-byte aligned; CAPR is 16 behind the read offset. */
        advance = (uint32_t)pkt_len + 4 + 3;
        rx_ptr = (rx_ptr + advance) & ~3u;
        rx_ptr %= RTL_RX_RING;
        pci_io_write16(iobase + RTL_CAPR, (uint16_t)(rx_ptr - 16));
    }
}

/* The sources unmasked at bring-up; TOK needs no work. */
#define RTL_ISR_HANDLED \
    (RTL_ISR_ROK | RTL_ISR_TOK | RTL_ISR_RER | RTL_ISR_TER | RTL_ISR_RXOVW | RTL_ISR_FOVW)

/**
 * The card's interrupt handler: service the card until its status reads
 * clear.
 *
 * The line may be shared, with the HDA controller among others, and a frame
 * that arrives after the status is acknowledged but before the ring is
 * drained sets ROK again. Reading and acknowledging in a loop until nothing
 * is pending leaves the line deasserted and the ring drained on return, so no
 * event waits for the next interrupt. The guard of 64 reads bounds the loop
 * should a status bit stay set. The interrupt router ends the interrupt at
 * its controller, so this handler sends no end of interrupt.
 *
 * An overflow is counted as a receive drop on every entry but logged only
 * once in a boot, so that sustained traffic cannot push the boot's lines out
 * of the log with repeated warnings. Draining the ring is the whole
 * recovery: the card reports the overflow instead of writing the frame that
 * would not fit, so the frames already in the ring are whole, and a reset
 * would lose frames that arrived since. A ring read out of step is caught by
 * the header check in the drain, which does reset. A receive error is
 * counted and a transmit error logged; TOK needs nothing, since the driver
 * does not follow transmit completion.
 */
static void rtl8139_irq(isr_params_t *params)
{
    uint16_t isr_val;
    int guard;

    (void)params;

    for (guard = 0; guard < 64; guard++) {
        isr_val = pci_io_read16(iobase + RTL_ISR);
        if ((isr_val & RTL_ISR_HANDLED) == 0) {
            break;
        }
        pci_io_write16(iobase + RTL_ISR, isr_val); /* bits written as 1 clear */

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
    }
}
