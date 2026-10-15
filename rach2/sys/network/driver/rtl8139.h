#ifndef _RTL8139_H_
#define _RTL8139_H_

#include "../../lib/kint.h"
#include "netdev.h"

/*
 * rtl8139.h: the registers of the Realtek RTL8139, the card
 * rach2/qemu_devices.sh gives QEMU unless `NIC` names another, and the
 * driver's entry points.
 *
 * QEMU attaches it with `-device rtl8139,netdev=net0`. BAR0 is an I/O
 * window, so every register below is an offset from the port the BAR gives
 * (`pci_bar_address`). The card sends from four transmit descriptors in
 * turn and receives into one ring buffer, each frame after a 4-byte header.
 */

/* Register offsets (I/O port, relative to the I/O base). */
#define RTL_MAC0    0x00 /* MAC address bytes 0-3 (read as 32-bit)  */
#define RTL_MAC4    0x04 /* MAC address bytes 4-5 (read as 16-bit)  */
#define RTL_MAR0    0x08 /* Multicast filter 0-3                    */
#define RTL_MAR4    0x0C /* Multicast filter 4-7                    */
#define RTL_TSD0    0x10 /* TX status descriptor 0 (32-bit)         */
#define RTL_TSAD0   0x20 /* TX start address 0 (32-bit, physical)   */
#define RTL_RBSTART 0x30 /* RX buffer start address (32-bit, phys)  */
#define RTL_CHIPCMD 0x37 /* Chip command: RE/TE/RST                 */
#define RTL_CAPR    0x38 /* Current address of packet read (16-bit) */
#define RTL_CBR     0x3A /* Current buffer address (16-bit, RO)     */
#define RTL_IMR     0x3C /* Interrupt mask (16-bit)                 */
#define RTL_ISR     0x3E /* Interrupt status (16-bit, write-1-clear)*/
#define RTL_TCR     0x40 /* TX configuration (32-bit)               */
#define RTL_RCR     0x44 /* RX configuration (32-bit)               */
#define RTL_9346CR  0x50 /* 93C46 command: 0xC0 config, 0x00 normal */
#define RTL_CONFIG1 0x52 /* Configuration register 1                */
#define RTL_MSR     0x58 /* Media status (8-bit)                    */
#define RTL_BMCR    0x62 /* Basic mode control (16-bit)             */

/* Media status bits. LINKB is inverted: it is set while the link is
   down. */
#define RTL_MSR_LINKB    0x04 /* 1 = no link */
#define RTL_MSR_SPEED_10 0x08 /* 1 = 10 Mb/s, 0 = 100 Mb/s */

/* Basic mode control: the duplex the PHY settled on. */
#define RTL_BMCR_DUPLEX 0x0100

/* Interrupt status / mask bits (RTL_ISR / RTL_IMR). */
#define RTL_ISR_ROK   0x0001 /* RX OK             */
#define RTL_ISR_RER   0x0002 /* RX error          */
#define RTL_ISR_TOK   0x0004 /* TX OK             */
#define RTL_ISR_TER   0x0008 /* TX error          */
#define RTL_ISR_RXOVW 0x0010 /* RX buffer overflow*/
#define RTL_ISR_FOVW  0x0040 /* RX FIFO overflow  */

/* Chip command bits (RTL_CHIPCMD). */
#define RTL_CMD_RST     0x10 /* Software reset (self-clearing)  */
#define RTL_CMD_RE      0x08 /* Receiver enable                 */
#define RTL_CMD_TE      0x04 /* Transmitter enable              */
#define RTL_CMD_RXEMPTY 0x01 /* RX ring empty (1 = empty)       */

/* TX status descriptor bits (RTL_TSD0). */
#define RTL_TSD_OWN 0x2000 /* Buffer-to-FIFO DMA done; slot reusable */
#define RTL_TSD_TOK 0x8000 /* Transmit completed OK                  */

/*
 * Receive configuration: accept every destination (AAP), this card's own
 * (APM), multicast (AM) and broadcast (AB); WRAP set, so a frame that
 * reaches the end of the ring goes on past it into the overhang instead of
 * wrapping to the start; an unlimited DMA burst; and the 8 KiB ring (RBLEN
 * 0). QEMU's user network has no other station, so accepting every
 * destination costs nothing there.
 */
#define RTL_RCR_VALUE (0xF | (1 << 7) | (7 << 8))

/* Transmit configuration: the standard 96-bit interframe gap and a
   1024-byte DMA burst. */
#define RTL_TCR_VALUE ((3 << 24) | (6 << 8))

/*
 * The receive buffer: the 8 KiB ring and the 16 bytes the card adds to its
 * length for RBLEN 0, then a 2 KiB overhang, so that a frame of the largest
 * size that starts near the end of the ring is whole in memory (WRAP set).
 */
#define RTL_RX_RING     8192
#define RTL_RX_BUF_SIZE (RTL_RX_RING + 16 + 2048)
#define RTL_TX_BUF_SIZE 1536 /* one max Ethernet frame */
#define RTL_TX_NUM      4    /* the card has four TX descriptors */

/* What a ring header's length may be. */
#define RTL_RX_EARLY   0xFFF0 /* length while the card writes the frame */
#define RTL_RX_PKT_MIN 18     /* Ethernet header (14) + CRC (4)        */
#define RTL_RX_PKT_MAX 1518   /* max frame (1514) + CRC (4)            */

/* The interface a card is brought up for. */
struct netdev;

/**
 * The driver table's `init` (netdev.h) for the one RTL8139 this driver
 * drives, the first 10EC:8139 function on the PCI bus. `nd->dev` is left
 * NULL.
 *
 * Returns `NETDEV_ABSENT` for any `nth` but 0 or when there is no such
 * function; `NETDEV_FAILED`, with `rtl8139_error` giving the reason, for a
 * card whose line is above 15 or that has no I/O window, a reset that did
 * not finish, or a receive buffer the boot allocator could not give; and
 * `NETDEV_UP` otherwise. Called by the probe at boot.
 */
int rtl8139_init(struct netdev *nd, int nth);

/**
 * The driver table's `tx`: copy one frame into the next of the four slots
 * and start it. `dev` is ignored.
 *
 * Returns `NETDEV_TX_DROPPED`, with a warning in the log, for a frame of
 * more than RTL_TX_BUF_SIZE bytes, and `NETDEV_TX_OK` otherwise. The slot is
 * not checked for a frame still being sent: QEMU's card finishes each one as
 * its length is written. Takes `net_lock`, and may be called with it held.
 */
int rtl8139_tx(void *dev, const uint8_t *data, uint16_t len);

/** The driver table's `error`: the reason the last `rtl8139_init` that found
 * no card or failed gave, a string constant. */
const char *rtl8139_error(void);

/** This driver's entry in the driver table (netdev.c). Its `poll` is NULL:
 * the card interrupts and delivers from its handler. */
extern const struct netdev_ops rtl8139_netdev;

#endif /* _RTL8139_H_ */
