#ifndef _RTL8168_H_
#define _RTL8168_H_

#include "../../lib/kint.h"
#include "netdev.h"

/*
 * rtl8168.h: the registers of the Realtek RTL8168GU (RTL8111GU) PCIe
 * gigabit chip, the Lenovo G50-70's card, and the driver's entry in the
 * driver table.
 *
 * The chip is not the RTL8139 with more registers: it has the C+
 * descriptor-ring design, its PHY and MAC configuration are behind four
 * indirect-access windows (ERI, MAC-OCP, PHY-OCP, EPHY), and QEMU does not
 * emulate it. The driver is therefore a port of one pinned reference, not a
 * reading of a datasheet: Linux v6.16, commit
 * 038d61fd642278bab63ee8ef722c50d10ab01e8f,
 * drivers/net/ethernet/realtek/r8169_main.c and r8169_phy_config.c, for the
 * chip that file calls RTL_GIGA_MAC_VER_42. Each register sequence in
 * rtl8168.c names the reference function it came from, so a reader can check
 * the port against its source, and tests/host/rtl8168_hosttest.c drives the
 * whole bring-up against a software model of the chip that checks the
 * sequence step by step.
 *
 * The card's messages are taken on a vector of the driver's own where the
 * machine routes them, with the timer tick as the fallback; on a machine
 * that cannot route them the tick alone serves the card. Not supported:
 * MSI-X, jumbo frames, every offload, RSS, VLAN, Wake-on-LAN and runtime
 * power management.
 */

/* Register offsets in the memory window (reference: enum rtl_registers). */
#define R8168_MAC0            0x00
#define R8168_MAC4            0x04
#define R8168_MAR0            0x08
#define R8168_COUNTERADDRLOW  0x10
#define R8168_COUNTERADDRHIGH 0x14
#define R8168_TXDESC_LO       0x20
#define R8168_TXDESC_HI       0x24
#define R8168_CHIPCMD         0x37
#define R8168_TXPOLL          0x38
#define R8168_INTRMASK        0x3c
#define R8168_INTRSTATUS      0x3e
/* The sources the driver takes as messages, the reference's set for this
 * family: a frame received or in error, a frame sent or in error, and the
 * link changing. The status register clears a bit when that bit is written. */
#define R8168_INTR_RXOK       0x0001
#define R8168_INTR_RXERR      0x0002
#define R8168_INTR_TXOK       0x0004
#define R8168_INTR_TXERR      0x0008
#define R8168_INTR_LINKCHG    0x0020
#define R8168_INTR_ENABLED \
    (R8168_INTR_RXOK | R8168_INTR_RXERR | R8168_INTR_TXOK | R8168_INTR_TXERR | R8168_INTR_LINKCHG)
#define R8168_TXCONFIG            0x40
/* CounterAddrLow's command bits (reference: CounterReset, CounterDump); the
 * block's address takes the other 26, so the block is 64-byte aligned. */
#define R8168_COUNTER_RESET       0x00000001u
#define R8168_COUNTER_DUMP        0x00000008u
/* The chip clears the dump bit when it has written the block. The reference
 * waits 10 ms in steps of 10 us (rtl8169_do_counters). */
#define R8168_COUNTERS_WAIT_STEPS 1000u
#define R8168_COUNTERS_WAIT_USEC  10u

/* The 64-byte block the chip fills with its tally counters (reference:
 * struct rtl8169_counters), little-endian, the 64-bit counts as two words.
 * The low words are what the reports show; the counts wrap at 32 bits as
 * the interface's own do. */
struct r8168_counters {
    uint32_t tx_packets_lo, tx_packets_hi;
    uint32_t rx_packets_lo, rx_packets_hi;
    uint32_t tx_errors_lo, tx_errors_hi;
    uint32_t rx_errors;
    uint16_t rx_missed;
    uint16_t align_errors;
    uint32_t tx_one_collision;
    uint32_t tx_multi_collision;
    uint32_t rx_unicast_lo, rx_unicast_hi;
    uint32_t rx_broadcast_lo, rx_broadcast_hi;
    uint32_t rx_multicast;
    uint16_t tx_aborted;
    uint16_t tx_underrun;
};
#define R8168_RXCONFIG        0x44
#define R8168_CFG9346         0x50
#define R8168_CONFIG2         0x53
#define R8168_CONFIG3         0x54
#define R8168_CONFIG5         0x56
#define R8168_PHYSTATUS       0x6c
#define R8168_CSIDR           0x64
#define R8168_CSIAR           0x68
#define R8168_ERIDR           0x70
#define R8168_ERIAR           0x74
#define R8168_EPHYAR          0x80
#define R8168_OCPDR           0xb0
#define R8168_GPHY_OCP        0xb8
#define R8168_MCU             0xd3
#define R8168_RXMAXSIZE       0xda
#define R8168_CPLUSCMD        0xe0
#define R8168_INTRMITIGATE    0xe2
#define R8168_RXDESC_LO       0xe4
#define R8168_RXDESC_HI       0xe8
#define R8168_MAXTXPACKETSIZE 0xec
#define R8168_MISC            0xf0
#define R8168_EEE_LED         0x1b

/* ChipCmd bits. */
#define R8168_CMD_RESET  0x10
#define R8168_CMD_RX_ENB 0x08
#define R8168_CMD_TX_ENB 0x04

/* Cfg9346. */
#define R8168_CFG9346_UNLOCK 0xc0
#define R8168_CFG9346_LOCK   0x00

/* MCU bits: the out-of-band state the firmware can leave behind, and the
 * link-list-ready bit the reset handshake waits on. */
#define R8168_MCU_NOW_IS_OOB    0x80
#define R8168_MCU_RXTX_EMPTY    0x30
#define R8168_MCU_LINK_LIST_RDY 0x02

/* MISC: the receive-data-valid gate. The receiver passes nothing to memory
 * while it is set. */
#define R8168_MISC_RXDV_GATED 0x00080000

/* TxConfig: the revision field, and the bits rtl_set_tx_config_registers
 * writes (DMA burst 7, interframe gap 3, auto FIFO). */
#define R8168_TXCFG_EMPTY     0x00000800
#define R8168_TXCFG_VALUE     0x03000780
#define R8168_XID_SHIFT       20
#define R8168_XID_MASK        0xfcf
#define R8168_XID_FAMILY_MASK 0x7cf
#define R8168_XID_8168GU      0x509

/* RxConfig for this chip version, and the accept bits set last. */
#define R8168_RXCFG_VALUE           0x0000cf00
#define R8168_RXCFG_ACCEPT_OK_MASK  0x0f
#define R8168_RXCFG_ACCEPT_ERR_MASK 0x30
#define R8168_RXCFG_ACCEPT          0x0e /* broadcast, multicast, own address */

/* CPlusCmd: the bits the reference preserves from the power-on value. */
#define R8168_CPCMD_MASK 0x2063

/* Indirect-access command bits. */
#define R8168_ERIAR_FLAG        0x80000000u
#define R8168_ERIAR_WRITE       0x80000000u
#define R8168_ERIAR_MASK_0001   0x1000u
#define R8168_ERIAR_MASK_0011   0x3000u
#define R8168_ERIAR_MASK_1111   0xf000u
#define R8168_OCP_FLAG          0x80000000u
#define R8168_CSIAR_FLAG        0x80000000u
#define R8168_CSIAR_BYTE_ENABLE 0x0000f000u

/* The descriptor rings. 64 entries each, where the reference has 256: the
 * kernel's directly mapped window is 32 MiB (HARD_MEMORY_LIMIT), and 64
 * receive buffers of the reference's size are already a megabyte. It is the
 * one number of the driver not taken from the reference, and the host
 * model sends bursts against it. */
#define R8168_RING_SIZE   64
#define R8168_RX_BUF_SIZE 16383 /* the reference's SZ_16K - 1 */
#define R8168_RX_MAX_SIZE 16384

/* Descriptor opts1 bits. */
#define R8168_DESC_OWN      0x80000000u
#define R8168_DESC_EOR      0x40000000u
#define R8168_DESC_FS       0x20000000u
#define R8168_DESC_LS       0x10000000u
#define R8168_DESC_LEN_MASK 0x00003fffu
#define R8168_DESC_RX_RES   0x00200000u /* receive error, reference RxRES */

/* The doorbell: normal-priority queue. */
#define R8168_TXPOLL_NPQ 0x40

/* The largest frame this driver sends: the stack's own transmit buffer is one
 * header plus one 1500-byte transfer unit (network/ethernet/ethernet.c), and
 * nothing here supports jumbo frames. */
#define R8168_FRAME_MAX 1514

/* How long a transmit waits for a full ring to free a descriptor: ten waits
 * of 100 microseconds, about a millisecond, in which a 100 Mb/s link sends
 * eight full frames, and short enough that a stuck chip does not hold the
 * caller long. */
#define R8168_TX_WAIT_STEPS 10u
#define R8168_TX_WAIT_USEC  100u

/* How long `stop` waits for the frames the chip has taken to be sent,
   twenty waits of 500 microseconds (`rtl8168_stop`). */
#define R8168_DRAIN_STEPS 20u
#define R8168_DRAIN_USEC  500u

/** One C+ descriptor: 16 bytes, little-endian, as the chip reads them. */
struct r8168_desc {
    uint32_t opts1;
    uint32_t opts2;
    uint32_t addr_lo;
    uint32_t addr_hi;
};

/** This driver's entry in the driver table (netdev.c). */
extern const struct netdev_ops rtl8168_netdev;

#endif /* _RTL8168_H_ */
