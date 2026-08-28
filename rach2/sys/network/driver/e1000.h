#ifndef _E1000_H_
#define _E1000_H_

#include "../../lib/kint.h"
#include "netdev.h"

/*
 * Intel 82540EM gigabit driver, the card QEMU attaches when nobody chooses.
 *
 * `qemu-system-i386 -device e1000` is this chip; `e1000-82540em` is that
 * device's own alias. It is therefore the card a person gets from a plain
 * QEMU command line, and until this driver existed such a machine booted with
 * no network and no explanation. The related `e1000e` device is the 82574L,
 * a different register interface, and is not this driver's.
 *
 * The chip is the descriptor-ring design in its plainest form: two rings of
 * 16-byte descriptors, a head the card owns and a tail the driver owns, and
 * a done bit written back per descriptor. It has no indirect-access windows
 * and no vendor firmware, which is what makes it the one to read first. The
 * register data below comes from the Intel PCI/PCI-X Family of Gigabit
 * Ethernet Controllers Software Developer's Manual, and every value this
 * driver writes is named rather than spelled as a number.
 *
 * Deliberately absent: jumbo frames, every checksum and segmentation
 * offload, MSI and MSI-X (this kernel is PIC-only), multiple queues, VLAN
 * handling, interrupt throttling, and runtime power management.
 */

/* The one device this driver claims. QEMU's `e1000` reports it. */
#define E1000_VENDOR 0x8086
#define E1000_DEVICE 0x100e

/* The register window the 82540EM declares in BAR0. The driver reads and
   writes nothing above RAH0; the whole window is mapped anyway, because that
   is what the card decodes and a partial mapping would fault on a read this
   driver does not yet make. */
#define E1000_BAR_WINDOW (128u * 1024u)

/* Registers, by offset in the memory window. The driver touches nothing
   above RAH0. */
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

/* Device control bits. */
#define E1000_CTRL_FD   0x00000001u /* full duplex */
#define E1000_CTRL_ASDE 0x00000020u /* auto speed detection */
#define E1000_CTRL_SLU  0x00000040u /* set link up */
#define E1000_CTRL_RST  0x04000000u /* device reset, self-clearing */
#define E1000_CTRL_VME  0x40000000u /* VLAN mode */
#define E1000_CTRL_PHY_RST 0x80000000u

/* Device status bits. The speed field is two bits: 0 is 10, 1 is 100, and
   both 2 and 3 mean 1000. */
#define E1000_STATUS_FD          0x00000001u /* full duplex */
#define E1000_STATUS_LU          0x00000002u /* link up */
#define E1000_STATUS_SPEED_MASK  0x000000c0u
#define E1000_STATUS_SPEED_SHIFT 6

/* EEPROM read protocol, in the 82540EM's field layout. The 82544 and later
   parts moved the address field and the done bit, so a chip that answers a
   read with DONE never set is refused rather than read with the wrong
   layout. */
#define E1000_EERD_START      0x00000001u
#define E1000_EERD_DONE       0x00000010u
#define E1000_EERD_ADDR_SHIFT 8
#define E1000_EERD_DATA_SHIFT 16
#define E1000_EEPROM_STEPS    100u
#define E1000_EEPROM_USEC     50u

/* The station address registers. AV marks RAH0 as holding a real address,
   which is how a card that loaded it from its own EEPROM at power-on is told
   apart from one whose registers are still clear. */
#define E1000_RAH_AV     0x80000000u
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

/* Transmit control. PSP asks the card to pad a short frame; this driver pads
   in software as well, because the two do not agree on every emulated card
   and a runt on the wire is the failure that would follow. */
#define E1000_TCTL_EN         0x00000002u
#define E1000_TCTL_PSP        0x00000008u /* pad short packets */
#define E1000_TCTL_CT_SHIFT   4           /* collision threshold */
#define E1000_TCTL_COLD_SHIFT 12          /* collision distance */
#define E1000_TCTL_CT_DEFAULT 0x0fu
#define E1000_TCTL_COLD_FD    0x40u /* full duplex distance, in byte times */

/* Transmit inter-packet gap, the copper values the reference driver uses:
   IPGT 8, IPGR1 8, IPGR2 6. QEMU ignores this register; a real chip does
   not. */
#define E1000_TIPG_IPGT        8u
#define E1000_TIPG_IPGR1       8u
#define E1000_TIPG_IPGR2       6u
#define E1000_TIPG_IPGR1_SHIFT 10
#define E1000_TIPG_IPGR2_SHIFT 20

/* Interrupt causes. The mask carries only what this driver acts on, which is
   why TXDW is not in it: transmit descriptors are reclaimed by the next
   transmit, so an interrupt per completion would be one this handler has
   nothing to do with. It is still named, because reading ICR reports every
   cause whatever the mask says. */
#define E1000_INT_TXDW   0x00000001u /* transmit descriptor written back */
#define E1000_INT_LSC    0x00000004u /* link status changed */
#define E1000_INT_RXDMT0 0x00000010u /* receive ring at its low threshold */
#define E1000_INT_RXO    0x00000040u /* receive overrun */
#define E1000_INT_RXT0   0x00000080u /* receive timer expired */
#define E1000_INT_MASK (E1000_INT_LSC | E1000_INT_RXDMT0 | E1000_INT_RXO | E1000_INT_RXT0)

/* Receive descriptor status and error bits. A descriptor can report DD and
   EOP and still carry an error, which is why both bytes are read. */
#define E1000_RXD_STAT_DD  0x01u /* descriptor done */
#define E1000_RXD_STAT_EOP 0x02u /* end of packet */
#define E1000_RXD_ERR_ANY  0xffu

/* Transmit descriptor command and status bits. RS is what makes the card
   write DD back, and without it a full ring could never be reclaimed. */
#define E1000_TXD_CMD_EOP  0x01u /* end of packet */
#define E1000_TXD_CMD_IFCS 0x02u /* insert the frame check sequence */
#define E1000_TXD_CMD_RS   0x08u /* report status */
#define E1000_TXD_STAT_DD  0x01u /* descriptor done */

/* The rings. 32 descriptors each, so both rings are 512 bytes, which
   satisfies the requirement that RDLEN and TDLEN be a multiple of 128. */
#define E1000_RING_SIZE 32
#define E1000_RING_BYTES (E1000_RING_SIZE * 16)

/* Receive buffers are the 2048 bytes RCTL_SZ_2048 selects. The largest frame
   this driver sends is one Ethernet header plus one 1500-byte transfer unit;
   the stack builds nothing larger (network/ethernet/ethernet.c). */
#define E1000_RX_BUF_SIZE 2048
#define E1000_TX_BUF_SIZE 1536
#define E1000_FRAME_MAX   1514
#define E1000_FRAME_MIN   14 /* an Ethernet header and nothing after it */

/* How long a transmit waits for a full ring to free its next descriptor:
   about a millisecond, the interval rtl8168.c settled on, short enough that a
   wedged card does not hold the caller. */
#define E1000_TX_WAIT_STEPS 10u
#define E1000_TX_WAIT_USEC  100u

/* How long the reset handshake is given to clear CTRL.RST. The manual asks
   for a wait of microseconds; this is far longer, because a budget that is
   too short is a bring-up that fails on one machine only. */
#define E1000_RESET_STEPS 100u
#define E1000_RESET_USEC  100u

/**
 * One legacy receive descriptor: 16 bytes, little-endian, as the card reads
 * them.
 *
 * The manual describes the second half as a 16-bit length, a 16-bit checksum,
 * a status byte, an error byte, and a 16-bit VLAN field. They are packed into
 * two 32-bit words here for the reason `struct r8168_desc` packs its own: the
 * layout is then stated by the shifts that read it rather than resting on
 * what the compiler does with adjacent narrow members.
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

/* The accessors take a volatile pointer because the rings are shared with
   the card; see the declaration in e1000.c. */
#define E1000_RXD_LEN(d)    ((d)->len_csum & 0xffffu)
#define E1000_RXD_STATUS(d) ((uint8_t)((d)->sta_err & 0xffu))
#define E1000_RXD_ERRORS(d) ((uint8_t)(((d)->sta_err >> 8) & 0xffu))
#define E1000_TXD_STATUS(d) ((uint8_t)((d)->upper & 0xffu))
#define E1000_TXD_LOWER(len, cmd) (((uint32_t)(len)) | ((uint32_t)(cmd) << 24))

/** This driver's entry in the device table (driver/netdev.h). */
extern const struct netdev_ops e1000_netdev;

#endif /* _E1000_H_ */
