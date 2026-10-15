#ifndef _E1000_H_
#define _E1000_H_

#include "../../lib/kint.h"
#include "netdev.h"

/*
 * e1000.h: the registers of the Intel 82540EM gigabit card, the card QEMU
 * attaches when no card is named, and the driver's entry in the table.
 *
 * `qemu-system-i386 -device e1000` is this chip; `e1000-82540em` is that
 * device's own alias. The related `e1000e` device is the 82574L, a different
 * register interface, and is not this driver's.
 *
 * The chip is the descriptor-ring design in its plainest form: two rings of
 * 16-byte descriptors, a head the card owns and a tail the driver owns, and
 * a done bit written back in each descriptor. It has no indirect-access
 * windows and no vendor firmware, which is why it is the one to read first.
 * The register data below comes from the Intel PCI/PCI-X Family of Gigabit
 * Ethernet Controllers Software Developer's Manual, and the bits this driver
 * writes are named here.
 *
 * Deliberately absent: jumbo frames, every checksum and segmentation
 * offload, MSI and MSI-X, multiple queues, VLAN handling, interrupt
 * throttling, and runtime power management.
 */

/* The one device this driver claims. QEMU's `e1000` reports it. */
#define E1000_VENDOR 0x8086
#define E1000_DEVICE 0x100e

/* The card decodes its registers through a memory window and an I/O window,
   and this driver needs both. The reset is issued through the I/O window
   because Intel's specification update records that on this part a memory
   write of `CTRL.RST` can be followed by a bridge padding write that takes a
   master abort while the controller is resetting, which some bridges promote
   to a fatal machine check. Linux's e1000 driver also resets the 82540
   through the I/O window. The I/O window is a pair: an address register at
   offset 0 and a data register at offset 4. */
#define E1000_IO_ADDR 0
#define E1000_IO_DATA 4

/* The memory window the 82540EM decodes, 128 KiB, which is mapped whole
   although the driver uses no register above RAH0. */
#define E1000_BAR_WINDOW (128u * 1024u)

/* Registers, by offset in the memory window. */
#define E1000_CTRL   0x0000 /* device control */
#define E1000_STATUS 0x0008 /* device status */
#define E1000_EERD   0x0014 /* EEPROM read */
#define E1000_ICR    0x00c0 /* interrupt cause, read to clear */
#define E1000_IMS    0x00d0 /* interrupt mask set */
#define E1000_IMC    0x00d8 /* interrupt mask clear */
#define E1000_RCTL   0x0100 /* receive control */
#define E1000_TCTL   0x0400 /* transmit control */
#define E1000_TIPG   0x0410 /* transmit inter-packet gap */
#define E1000_RDBAL  0x2800 /* receive ring base, low 32 bits */
#define E1000_RDBAH  0x2804 /* receive ring base, high 32 bits */
#define E1000_RDLEN  0x2808 /* receive ring length in bytes */
#define E1000_RDH    0x2810 /* receive head, the card's index */
#define E1000_RDT    0x2818 /* receive tail, this driver's index */
#define E1000_TDBAL  0x3800 /* transmit ring base, low 32 bits */
#define E1000_TDBAH  0x3804 /* transmit ring base, high 32 bits */
#define E1000_TDLEN  0x3808 /* transmit ring length in bytes */
#define E1000_TDH    0x3810 /* transmit head, the card's index */
#define E1000_TDT    0x3818 /* transmit tail, this driver's index */
#define E1000_MTA    0x5200 /* multicast table array, 128 entries */
#define E1000_RAL0   0x5400 /* station address, low 32 bits */
#define E1000_RAH0   0x5404 /* station address, high 16 bits and flags */

/* Device control bits. FRCSPD and FRCDPLX force the speed and duplex the MAC
   uses; auto-speed detection is only permitted while both are clear, and
   their power-on values come from the EEPROM, so a driver that asks for
   detection must clear them rather than assume. */
#define E1000_CTRL_FD      0x00000001u /* full duplex */
#define E1000_CTRL_ASDE    0x00000020u /* auto speed detection */
#define E1000_CTRL_SLU     0x00000040u /* set link up */
#define E1000_CTRL_FRCSPD  0x00000800u /* force speed */
#define E1000_CTRL_FRCDPLX 0x00001000u /* force duplex */
#define E1000_CTRL_RST     0x04000000u /* device reset, self-clearing */
#define E1000_CTRL_VME     0x40000000u /* VLAN mode */
#define E1000_CTRL_PHY_RST 0x80000000u /* PHY reset */

/* Device status bits. The speed field is two bits: 0 is 10, 1 is 100, and
   both 2 and 3 mean 1000. */
#define E1000_STATUS_FD          0x00000001u /* full duplex */
#define E1000_STATUS_LU          0x00000002u /* link up */
#define E1000_STATUS_SPEED_MASK  0x000000c0u
#define E1000_STATUS_SPEED_SHIFT 6

/* EEPROM read protocol, in the 82540EM's field layout. The 82541 and 82547
   moved the address field and the done bit, so a chip that answers a read
   with DONE never set is refused rather than read with the wrong layout. */
#define E1000_EERD_START      0x00000001u
#define E1000_EERD_DONE       0x00000010u
#define E1000_EERD_ADDR_SHIFT 8
#define E1000_EERD_DATA_SHIFT 16
#define E1000_EEPROM_STEPS    100u
#define E1000_EEPROM_USEC     50u

/* The station address registers. AV, address valid, marks RAH0 as holding
   an address, which is how a card that loaded it from its own EEPROM at
   power-on is told apart from one whose registers are still clear. */
#define E1000_RAH_AV      0x80000000u
#define E1000_MTA_ENTRIES 128u

/* Receive control. Broadcast is accepted because ARP and DHCP need it;
   promiscuous unicast is not set, so the card filters to RAL0/RAH0. SECRC
   strips the frame check sequence, which is what makes a descriptor's length
   the frame's length with no arithmetic. */
#define E1000_RCTL_EN      0x00000002u
#define E1000_RCTL_UPE     0x00000008u /* unicast promiscuous */
#define E1000_RCTL_MPE     0x00000010u /* multicast promiscuous */
#define E1000_RCTL_LPE     0x00000020u /* long packet enable */
#define E1000_RCTL_BAM     0x00008000u /* broadcast accept */
#define E1000_RCTL_SZ_2048 0x00000000u /* BSIZE 00 with BSEX clear */
#define E1000_RCTL_SECRC   0x04000000u /* strip the frame check sequence */

/* Transmit control. PSP asks the card to pad a short frame, which the
   driver also pads itself (`e1000_tx_frame`). */
#define E1000_TCTL_EN         0x00000002u
#define E1000_TCTL_PSP        0x00000008u /* pad short packets */
/* Retransmit on late collision. With this clear a late collision on a
   half-duplex link is reported as a successful transmission rather than
   retried, which is a frame lost with nothing to show for it. */
#define E1000_TCTL_RTLC       0x01000000u
#define E1000_TCTL_CT_SHIFT   4  /* collision threshold */
#define E1000_TCTL_COLD_SHIFT 12 /* collision distance */
#define E1000_TCTL_CT_DEFAULT 0x0fu
#define E1000_TCTL_COLD_FD    0x40u /* full duplex distance, in byte times */

/* Transmit inter-packet gap, the copper values Linux's e1000 driver uses:
   IPGT 8, IPGR1 8, IPGR2 6. QEMU ignores this register; the chip does
   not. */
#define E1000_TIPG_IPGT        8u
#define E1000_TIPG_IPGR1       8u
#define E1000_TIPG_IPGR2       6u
#define E1000_TIPG_IPGR1_SHIFT 10
#define E1000_TIPG_IPGR2_SHIFT 20

/* Interrupt causes. The mask has the receive causes and the link change.
   TXDW is left out of it, since transmit descriptors are reclaimed by the
   next transmit and an interrupt for each completion would find nothing to
   do; it is named because reading ICR reports every cause whatever the
   mask. */
#define E1000_INT_TXDW   0x00000001u /* transmit descriptor written back */
#define E1000_INT_LSC    0x00000004u /* link status changed */
#define E1000_INT_RXDMT0 0x00000010u /* receive ring at its low threshold */
#define E1000_INT_RXO    0x00000040u /* receive overrun */
#define E1000_INT_RXT0   0x00000080u /* receive timer expired */
#define E1000_INT_MASK   (E1000_INT_LSC | E1000_INT_RXDMT0 | E1000_INT_RXO | E1000_INT_RXT0)

/* Receive descriptor status and error bits. A descriptor can report DD and
   EOP and still have an error bit set, which is why both bytes are read. */
#define E1000_RXD_STAT_DD  0x01u /* descriptor done */
#define E1000_RXD_STAT_EOP 0x02u /* end of packet */
#define E1000_RXD_ERR_ANY  0xffu

/* Transmit descriptor command and status bits. RS is what makes the card
   write DD back, and without it a full ring could never be reclaimed. */
#define E1000_TXD_CMD_EOP  0x01u /* end of packet */
#define E1000_TXD_CMD_IFCS 0x02u /* insert the frame check sequence */
#define E1000_TXD_CMD_RS   0x08u /* report status */
#define E1000_TXD_STAT_DD  0x01u /* descriptor done */

/* The rings. 32 descriptors each, so each ring is 512 bytes, which
   satisfies the requirement that RDLEN and TDLEN be a multiple of 128. */
#define E1000_RING_SIZE  32
#define E1000_RING_BYTES (E1000_RING_SIZE * 16)

/* Receive buffers are the 2048 bytes RCTL_SZ_2048 selects. The largest frame
   this driver sends is one Ethernet header plus one 1500-byte transfer unit;
   the stack builds nothing larger (network/ethernet/ethernet.c). */
#define E1000_RX_BUF_SIZE 2048
#define E1000_TX_BUF_SIZE 1536
#define E1000_FRAME_MAX   1514
#define E1000_FRAME_MIN   14 /* an Ethernet header and nothing after it */

/* How long a transmit waits for a full ring to free its next descriptor:
   ten waits of 100 microseconds, about a millisecond, as in rtl8168.c, short
   enough that a stuck card does not hold the caller long. */
#define E1000_TX_WAIT_STEPS 10u
#define E1000_TX_WAIT_USEC  100u

/* How long `stop` waits for the frames the card has taken to be sent,
   twenty waits of 500 microseconds (`e1000_stop`). */
#define E1000_DRAIN_STEPS 20u
#define E1000_DRAIN_USEC  500u

/* How long the card is given to clear CTRL.RST, 100 waits of 100
   microseconds. The manual asks for a wait of microseconds; this is far
   longer, because a limit that is too short is a bring-up that fails on one
   machine only. */
#define E1000_RESET_STEPS 100u
#define E1000_RESET_USEC  100u

/* How long a card the firmware left running is given to finish a transfer
   already begun before it is reset. Linux's e1000 driver waits 10 ms at the
   same point. */
#define E1000_QUIESCE_USEC 10000u

/**
 * One legacy receive descriptor: 16 bytes, little-endian, as the card reads
 * them.
 *
 * The manual describes the second half as a 16-bit length, a 16-bit checksum,
 * a status byte, an error byte, and a 16-bit VLAN field. They are kept as two
 * 32-bit words here and read with shifts, so the layout does not depend on
 * how a compiler places adjacent narrow members.
 */
struct e1000_rx_desc {
    uint32_t addr_lo;
    uint32_t addr_hi;
    uint32_t len_csum; /* length in bits 0..15, checksum in 16..31 */
    uint32_t sta_err;  /* status in 0..7, errors in 8..15, VLAN in 16..31 */
};

/** One legacy transmit descriptor, packed the same way. */
struct e1000_tx_desc {
    uint32_t addr_lo;
    uint32_t addr_hi;
    uint32_t lower; /* length in 0..15, offload offset in 16..23, command in 24..31 */
    uint32_t upper; /* status in 0..7, offload start in 8..15, VLAN in 16..31 */
};

/* The fields of the packed words. The accessors are given the rings'
   descriptors, which are volatile because the card writes them (`struct
   e1000` in e1000.c). */
#define E1000_RXD_LEN(d)          ((d)->len_csum & 0xffffu)
#define E1000_RXD_STATUS(d)       ((uint8_t)((d)->sta_err & 0xffu))
#define E1000_RXD_ERRORS(d)       ((uint8_t)(((d)->sta_err >> 8) & 0xffu))
#define E1000_TXD_STATUS(d)       ((uint8_t)((d)->upper & 0xffu))
#define E1000_TXD_LOWER(len, cmd) (((uint32_t)(len)) | ((uint32_t)(cmd) << 24))

/** This driver's entry in the driver table (netdev.c). */
extern const struct netdev_ops e1000_netdev;

#endif /* _E1000_H_ */
