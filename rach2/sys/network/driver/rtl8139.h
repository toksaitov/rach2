#ifndef _RTL8139_H_
#define _RTL8139_H_

#include "../../lib/kint.h"
#include "netdev.h"

/*
 * Realtek RTL8139 PCI NIC driver.
 *
 * QEMU exposes this card with "-device rtl8139,netdev=net0". BAR0 is an I/O
 * BAR, so every register below is an offset from the I/O base returned by
 * pci_bar_address(). The card uses four TX descriptors and a single linear RX
 * ring buffer.
 */

/* Register offsets (I/O port, relative to the I/O base). */
#define RTL_MAC0        0x00    /* MAC address bytes 0-3 (read as 32-bit)  */
#define RTL_MAC4        0x04    /* MAC address bytes 4-5 (read as 16-bit)  */
#define RTL_MAR0        0x08    /* Multicast filter 0-3                    */
#define RTL_MAR4        0x0C    /* Multicast filter 4-7                    */
#define RTL_TSD0        0x10    /* TX status descriptor 0 (32-bit)         */
#define RTL_TSAD0       0x20    /* TX start address 0 (32-bit, physical)   */
#define RTL_RBSTART     0x30    /* RX buffer start address (32-bit, phys)  */
#define RTL_CHIPCMD     0x37    /* Chip command: RE/TE/RST                 */
#define RTL_CAPR        0x38    /* Current address of packet read (16-bit) */
#define RTL_CBR         0x3A    /* Current buffer address (16-bit, RO)     */
#define RTL_IMR         0x3C    /* Interrupt mask (16-bit)                 */
#define RTL_ISR         0x3E    /* Interrupt status (16-bit, write-1-clear)*/
#define RTL_TCR         0x40    /* TX configuration (32-bit)               */
#define RTL_RCR         0x44    /* RX configuration (32-bit)               */
#define RTL_9346CR      0x50    /* 93C46 command: 0xC0 config, 0x00 normal */
#define RTL_CONFIG1     0x52    /* Configuration register 1                */

/* Interrupt status / mask bits (RTL_ISR / RTL_IMR). */
#define RTL_ISR_ROK     0x0001  /* RX OK             */
#define RTL_ISR_RER     0x0002  /* RX error          */
#define RTL_ISR_TOK     0x0004  /* TX OK             */
#define RTL_ISR_TER     0x0008  /* TX error          */
#define RTL_ISR_RXOVW   0x0010  /* RX buffer overflow*/
#define RTL_ISR_FOVW    0x0040  /* RX FIFO overflow  */

/* Chip command bits (RTL_CHIPCMD). */
#define RTL_CMD_RST     0x10    /* Software reset (self-clearing)  */
#define RTL_CMD_RE      0x08    /* Receiver enable                 */
#define RTL_CMD_TE      0x04    /* Transmitter enable              */
#define RTL_CMD_RXEMPTY 0x01    /* RX ring empty (1 = empty)       */

/* TX status descriptor bits (RTL_TSD0). */
#define RTL_TSD_OWN     0x2000  /* Buffer-to-FIFO DMA done; slot reusable */
#define RTL_TSD_TOK     0x8000  /* Transmit completed OK                  */

/*
 * RX configuration: accept unicast (APM), broadcast (AB), and all packets
 * (AAP/promiscuous, so we also see ARP aimed at the gateway), WRAP=1 so the
 * chip's DMA wraps the ring, and an unlimited DMA burst. Promiscuous is fine
 * on QEMU's SLIRP network.
 */
#define RTL_RCR_VALUE   (0xF | (1 << 7) | (7 << 8))

/* TX configuration: standard 96-bit interframe gap, 1024-byte DMA burst. */
#define RTL_TCR_VALUE   ((3 << 24) | (6 << 8))

/*
 * RX ring is 8 KB plus 16 bytes for the CAPR offset convention plus a 2 KB
 * overhang so a maximum frame that wraps the ring end still lands in one
 * contiguous span (WRAP=1).
 */
#define RTL_RX_RING      8192
#define RTL_RX_BUF_SIZE  (RTL_RX_RING + 16 + 2048)
#define RTL_TX_BUF_SIZE  1536   /* one max Ethernet frame */
#define RTL_TX_NUM       4      /* the card has four TX descriptors */

/* RX ring header validation. */
#define RTL_RX_EARLY    0xFFF0  /* length while the chip is still DMAing */
#define RTL_RX_PKT_MIN  18      /* Ethernet header (14) + CRC (4)        */
#define RTL_RX_PKT_MAX  1518    /* max frame (1514) + CRC (4)            */

/** Probe and bring up the card; 1 on success, 0 if absent or bring-up fails. */
int rtl8139_init(void);

/** Transmit a raw Ethernet frame (destination MAC through payload, no FCS; the
 *  chip appends the CRC). len must be <= RTL_TX_BUF_SIZE. */
void rtl8139_tx(const uint8_t *data, uint16_t len);

/** Configured I/O base. */
uint16_t rtl8139_iobase(void);

/** Configured IRQ line. */
uint8_t  rtl8139_irq_line(void);

/** Reason the most recent rtl8139_init() returned 0; valid only after a 0 return. */
const char *rtl8139_error(void);

/** This driver's entry in the device table (see driver/netdev.h). Its `poll`
 *  is NULL: the card takes an interrupt and delivers from its handler. */
extern const struct netdev_ops rtl8139_netdev;

#endif /* _RTL8139_H_ */
