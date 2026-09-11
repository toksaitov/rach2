#ifndef _RTL8168_H_
#define _RTL8168_H_

#include "../../lib/kint.h"
#include "netdev.h"

/*
 * Realtek RTL8168GU (RTL8111GU) PCIe gigabit driver.
 *
 * The chip in the Lenovo G50-70 this kernel is being carried to. It is not the
 * RTL8139 with more registers: it is the C+ descriptor-ring design, its PHY
 * and MAC configuration live behind four different indirect-access windows
 * (ERI, MAC-OCP, PHY-OCP, EPHY), and QEMU does not emulate it, so this driver
 * cannot be tested before it meets the hardware.
 *
 * It is therefore a port of one pinned reference, not a reading of a
 * datasheet: Linux v6.16, commit 038d61fd642278bab63ee8ef722c50d10ab01e8f,
 * drivers/net/ethernet/realtek/r8169_main.c and r8169_phy_config.c, for the
 * chip that file calls RTL_GIGA_MAC_VER_42. Every register sequence below
 * names the reference function it came from, so a reader can check the port
 * against its source, and tests/host/rtl8168_hosttest.c drives the whole
 * bring-up against a software model of the chip that asserts the sequence
 * step by step.
 *
 * Deliberately absent, for the whole of this milestone: MSI and MSI-X (this
 * kernel is PIC-only), device interrupts of any kind (the mask stays zero and
 * the driver is polled from the timer tick), jumbo frames, every offload,
 * RSS, VLAN, Wake-on-LAN and runtime power management.
 */

/* Register offsets in the memory window (reference: enum rtl_registers). */
#define R8168_MAC0            0x00
#define R8168_MAC4            0x04
#define R8168_MAR0            0x08
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
#define R8168_INTR_ENABLED    (R8168_INTR_RXOK | R8168_INTR_RXERR | R8168_INTR_TXOK | \
                               R8168_INTR_TXERR | R8168_INTR_LINKCHG)
#define R8168_TXCONFIG        0x40
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

/* MISC: the receive-data-valid gate. RX stays dead while this is set. */
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
#define R8168_RXCFG_ACCEPT          0x0e /* broadcast, multicast, our own address */

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

/* The descriptor ring. 64 entries, not the reference's 256: this
 * kernel's whole direct map is 32 MiB, and 64 entries with the reference's
 * buffer size already cost a megabyte. The deviation is the one number in
 * this driver the reference does not vouch for, so the model burst-tests it
 * and the laptop measures it (dev-log/24-metal/plan.md). */
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

/* How long a transmit will wait for a full ring to free a descriptor: about a
 * millisecond, which is a tick's worth of chip time at 100 Mbps and short
 * enough that a wedged chip does not hold the caller. */
#define R8168_TX_WAIT_STEPS 10u
#define R8168_TX_WAIT_USEC  100u

/* How long `stop` waits for what the chip already accepted to reach the wire
   before it silences the chip. */
#define R8168_DRAIN_STEPS 20u
#define R8168_DRAIN_USEC  500u

/** One C+ descriptor: 16 bytes, little-endian, as the chip reads them. */
struct r8168_desc {
    uint32_t opts1;
    uint32_t opts2;
    uint32_t addr_lo;
    uint32_t addr_hi;
};

/** This driver's entry in the device table (driver/netdev.h). */
extern const struct netdev_ops rtl8168_netdev;

#endif /* _RTL8168_H_ */
