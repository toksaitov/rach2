/*
 * rtl8168.c: the RTL8168GU driver, ported from a pinned reference.
 *
 * QEMU does not emulate the chip, so this file was written against Linux
 * v6.16 (commit 038d61fd642278bab63ee8ef722c50d10ab01e8f),
 * drivers/net/ethernet/realtek/r8169_main.c and r8169_phy_config.c, for the
 * version that file calls RTL_GIGA_MAC_VER_42, and every sequence below names
 * the reference function it was taken from. What the reference does through
 * phylib and a firmware blob, this does directly and without firmware. The
 * card is served from the timer tick, and through its messages where the
 * machine can route them.
 *
 * Two deliberate departures from the reference, both stated here because
 * neither can be discovered by reading the sequences alone:
 *
 * - Link state is read from PHYstatus (0x6c). The reference declares that
 *   register and its bits and then never reads it, because phylib reaches the
 *   PHY over MDIO instead. A kernel with no phylib has no such path, so this
 *   driver reads the MAC's own summary of the link. That is a documented
 *   register used in a way the reference does not exercise.
 * - The PHY is powered up here. The reference suspends the PHY at the end of
 *   probe and lets phy_resume wake it at open; with no phylib nothing would
 *   ever clear BMCR power-down, and the MAC would be configured and silent.
 *
 * All register access goes through an indirection (`io`) so the host test
 * can drive the whole bring-up against a software model of the chip; the
 * default binding is plain MMIO on the mapped window.
 */

#include "../../klog.h"
#include "../../lib/kstring.h"
#include "rtl8168.h"
#include "../net.h"
#include "../ethernet/ethernet.h"
#include "../../serial.h"
#include "../../udelay.h"
#include "../../pci.h"
#include "../../mm/paging.h"
#include "../../irq.h"

/* Register access, behind an indirection the host test can replace. */

/**
 * The chip's register window and the short waits its handshakes need.
 *
 * `udelay` is a floor, not a deadline: every use below is a poll interval or a
 * settle time the reference expresses with fsleep/udelay, and waiting longer
 * than asked is always safe. The real binding is `k_udelay`, which measures
 * against the timer chip; a counted loop cannot promise a duration on a
 * processor whose speed the kernel never learns, and a settle time that comes
 * up short leaves this chip half-configured.
 */
struct r8168_io {
    uint8_t (*r8)(uint32_t off);
    uint16_t (*r16)(uint32_t off);
    uint32_t (*r32)(uint32_t off);
    void (*w8)(uint32_t off, uint8_t value);
    void (*w16)(uint32_t off, uint16_t value);
    void (*w32)(uint32_t off, uint32_t value);
    void (*udelay)(uint32_t usec);
};

/* The mapped window; the real accessors read and write through it. */
static uintptr_t r8168_mmio;

static uint8_t mmio_r8(uint32_t off)
{
    return mmio_read8(r8168_mmio + off);
}

static uint16_t mmio_r16(uint32_t off)
{
    return mmio_read16(r8168_mmio + off);
}

static uint32_t mmio_r32(uint32_t off)
{
    return mmio_read32(r8168_mmio + off);
}

static void mmio_w8(uint32_t off, uint8_t value)
{
    mmio_write8(r8168_mmio + off, value);
}

static void mmio_w16(uint32_t off, uint16_t value)
{
    mmio_write16(r8168_mmio + off, value);
}

static void mmio_w32(uint32_t off, uint32_t value)
{
    mmio_write32(r8168_mmio + off, value);
}

static const struct r8168_io mmio_io = {mmio_r8,  mmio_r16, mmio_r32, mmio_w8,
                                        mmio_w16, mmio_w32, k_udelay};

/* The binding in force. The host test points this at its model. */
static const struct r8168_io *io = &mmio_io;

/* Driver state. */

static pci_device_t r8168_pci;
static uint32_t r8168_xid;
static uint16_t r8168_cp_cmd;
static const char *r8168_err = "no RTL8168 present";
static int r8168_up;
static void r8168_log_link(void);
static void r8168_note_sources(uint16_t status);

/* The PHY page base the MDIO shim is on, the reference's tp->ocp_base. */
#define R8168_OCP_STD_PHY_BASE 0xa400
static uint32_t r8168_ocp_base = R8168_OCP_STD_PHY_BASE;

/* Counters the diagnostic line reports; the polling budget is judged by them
 * (dev-log/24-metal/plan.md), so they are part of the contract, not decoration.
 */
static uint32_t r8168_rx_packets;
static uint32_t r8168_rx_errors;
static uint32_t r8168_rx_dropped;
static uint32_t r8168_tx_packets;
static uint32_t r8168_tx_stalls;

/* The card's messages: the vector they arrive on, or -1 while the driver is
 * polled; how many arrived; how many found nothing to do; and how many storms
 * there were, runs of R8168_STORM_ENTRIES entries that found a source the
 * acknowledgement does not clear, after which the sources stay masked and
 * the timer's pump alone drives the card. */
static int r8168_msi_vector = -1;
static void r8168_interrupts_enable(void);
static uint32_t r8168_msi_entries;
static uint32_t r8168_msi_idle;
static uint32_t r8168_msi_deferred;  /* entries that left work to the tick */
static uint32_t r8168_txerr_reports; /* status words with the transmit error source set */
static uint32_t r8168_rxerr_reports; /* status words with the receive error source set */
static uint32_t r8168_msi_storms;
static uint32_t r8168_msi_stuck; /* entries in a row: status left set, no work */
static uint32_t r8168_tick_work; /* frames the tick handled with messages on */

/*
 * The rings and their buffers, in .bss, which this kernel's image places
 * below the 32 MiB window's top and so below 4 GiB: what goes into the
 * descriptor is the physical address of the array, `virt_to_phys` of it,
 * with the high half 0. The rings are 256-byte aligned as the reference
 * requires.
 *
 * The rings are volatile and the buffers are not, the same distinction
 * e1000.c draws: the chip writes ownership back into a descriptor while this
 * driver is reading it, and an ownership bit the compiler kept in a register
 * would be a wait that never ends. The kernel builds this file without
 * optimization, where that cannot happen, but the host model builds the same
 * file optimized, so the type says what is true rather than resting on a
 * flag. A buffer is read only after its descriptor changed hands, so making a
 * megabyte of buffer volatile would slow every copy to no purpose.
 */
static volatile struct r8168_desc r8168_tx_ring[R8168_RING_SIZE] __attribute__((aligned(256)));
static volatile struct r8168_desc r8168_rx_ring[R8168_RING_SIZE] __attribute__((aligned(256)));
static uint8_t r8168_rx_buf[R8168_RING_SIZE][R8168_RX_BUF_SIZE + 1] __attribute__((aligned(8)));
static uint8_t r8168_tx_buf[R8168_RING_SIZE][R8168_FRAME_MAX] __attribute__((aligned(8)));
/* The block the chip writes its tally counters into on request. */
static struct r8168_counters r8168_counters __attribute__((aligned(64)));
static int r8168_counters_command(uint32_t command);

static uint32_t r8168_tx_head; /* next descriptor to fill */
static uint32_t r8168_tx_tail; /* oldest descriptor not yet reclaimed */
static uint32_t r8168_rx_cur;  /* next descriptor to inspect */

/* Bounded polling, the reference's rtl_loop_wait. */

/**
 * Poll `mask` at `reg` until every bit of it reaches `high`, or give up.
 *
 * The condition is ALL of the mask, not any of it, which is what the
 * reference's conditions say and the only reading that is right for a
 * multi-bit mask: the FIFO drain waits on two bits, and one FIFO empty while
 * the other still has data is not a drained chip. For a single-bit mask the
 * two readings are the same.
 *
 * The reference checks the condition BEFORE the first delay and up to `n`
 * times in all; a handshake that is already complete costs nothing.
 * Return zero on timeout. Initialization callers must either use a documented
 * fallback or fail. Shutdown continues to disable bus mastering.
 *
 * @return 1 when the condition held, 0 when the budget ran out.
 */
static int r8168_wait32(uint32_t reg, uint32_t mask, int high, uint32_t usec, uint32_t n)
{
    uint32_t i;

    for (i = 0; i < n; i++) {
        uint32_t v = io->r32(reg) & mask;
        int now = (v == (high ? mask : 0u)) ? 1 : 0;

        if (now) {
            return 1;
        }
        io->udelay(usec);
    }
    return 0;
}

/** As r8168_wait32, over an 8-bit register. */
static int r8168_wait8(uint32_t reg, uint8_t mask, int high, uint32_t usec, uint32_t n)
{
    uint32_t i;

    for (i = 0; i < n; i++) {
        uint8_t v = (uint8_t)(io->r8(reg) & mask);
        int now = (v == (high ? mask : 0)) ? 1 : 0;

        if (now) {
            return 1;
        }
        io->udelay(usec);
    }
    return 0;
}

/* The indirect-access windows. */

/**
 * Read the ERI dword at `addr` (reference: rtl_eri_read).
 *
 * A timeout is reported, not folded into the value: the reference
 * returns ~0 on timeout and its read-modify-write helpers feed that straight
 * back into the chip, which writes 0xffffffff into a configuration register.
 * This port refuses instead, and `*ok` says which happened.
 */
static uint32_t r8168_eri_read(uint32_t addr, int *ok)
{
    io->w32(R8168_ERIAR, R8168_ERIAR_MASK_1111 | addr);
    if (!r8168_wait32(R8168_ERIAR, R8168_ERIAR_FLAG, 1, 100, 100)) {
        *ok = 0;
        return 0;
    }
    *ok = 1;
    return io->r32(R8168_ERIDR);
}

/** Write `value` to the ERI dword at `addr` under `mask` (rtl_eri_write). */
static int r8168_eri_write(uint32_t addr, uint32_t mask, uint32_t value)
{
    io->w32(R8168_ERIDR, value);
    io->w32(R8168_ERIAR, R8168_ERIAR_WRITE | mask | addr);
    return r8168_wait32(R8168_ERIAR, R8168_ERIAR_FLAG, 0, 100, 100);
}

/** Clear `clear` and set `set` in the ERI dword at `addr` (rtl_w0w1_eri). */
static int r8168_eri_rmw(uint32_t addr, uint32_t clear, uint32_t set)
{
    int ok = 0;
    uint32_t v = r8168_eri_read(addr, &ok);

    if (!ok) {
        return 0;
    }
    return r8168_eri_write(addr, R8168_ERIAR_MASK_1111, (v & ~clear) | set);
}

/** Read the MAC-OCP word at `reg` (r8168_mac_ocp_read). No completion poll. */
static uint16_t r8168_mac_ocp_read(uint32_t reg)
{
    io->w32(R8168_OCPDR, reg << 15);
    return (uint16_t)(io->r32(R8168_OCPDR) & 0xffff);
}

/** Write the MAC-OCP word at `reg` (r8168_mac_ocp_write). */
static void r8168_mac_ocp_write(uint32_t reg, uint16_t data)
{
    io->w32(R8168_OCPDR, R8168_OCP_FLAG | (reg << 15) | (uint32_t)data);
}

/** Clear `clear` and set `set` in the MAC-OCP word (r8168_mac_ocp_modify). */
static void r8168_mac_ocp_modify(uint32_t reg, uint16_t clear, uint16_t set)
{
    uint16_t v = r8168_mac_ocp_read(reg);

    r8168_mac_ocp_write(reg, (uint16_t)((v & ~clear) | set));
}

/** Read the PHY-OCP word at `reg` (r8168_phy_ocp_read); `*ok` reports
 * timeout. */
static uint16_t r8168_phy_ocp_read(uint32_t reg, int *ok)
{
    io->w32(R8168_GPHY_OCP, reg << 15);
    if (!r8168_wait32(R8168_GPHY_OCP, R8168_OCP_FLAG, 1, 25, 10)) {
        *ok = 0;
        return 0;
    }
    *ok = 1;
    return (uint16_t)(io->r32(R8168_GPHY_OCP) & 0xffff);
}

/** Write the PHY-OCP word at `reg` (r8168_phy_ocp_write). */
static int r8168_phy_ocp_write(uint32_t reg, uint16_t data)
{
    io->w32(R8168_GPHY_OCP, R8168_OCP_FLAG | (reg << 15) | (uint32_t)data);
    return r8168_wait32(R8168_GPHY_OCP, R8168_OCP_FLAG, 0, 25, 10);
}

/**
 * Select the PHY register page.
 *
 * Register 0x1f never reaches the chip: writing it moves the OCP base this
 * shim adds, exactly as the reference's `ocp_base` does. Nothing is issued, so
 * there is nothing here that can fail; the register access that follows is
 * what can.
 */
static void r8168_mdio_page(uint16_t page)
{
    r8168_ocp_base = page ? ((uint32_t)page << 4) : R8168_OCP_STD_PHY_BASE;
}

/**
 * Write an MII register through the PHY-OCP window (r8168g_mdio_write).
 *
 * On any page but the standard one the register number is biased by 0x10,
 * which is the reference's convention for the paged register file.
 */
static int r8168_mdio_write(uint32_t reg, uint16_t value)
{
    if (reg == 0x1f) {
        r8168_mdio_page(value);
        return 1;
    }
    if (r8168_ocp_base != R8168_OCP_STD_PHY_BASE) {
        reg -= 0x10;
    }
    return r8168_phy_ocp_write(r8168_ocp_base + reg * 2, value);
}

/** Read an MII register through the PHY-OCP window (r8168g_mdio_read). */
static uint16_t r8168_mdio_read(uint32_t reg, int *ok)
{
    if (reg == 0x1f) {
        *ok = 1;
        return (uint16_t)(r8168_ocp_base == R8168_OCP_STD_PHY_BASE ? 0 : (r8168_ocp_base >> 4));
    }
    if (r8168_ocp_base != R8168_OCP_STD_PHY_BASE) {
        reg -= 0x10;
    }
    return r8168_phy_ocp_read(r8168_ocp_base + reg * 2, ok);
}

/** Read the EPHY word at `reg` (rtl_ephy_read); `*ok` reports timeout. */
static uint16_t r8168_ephy_read(uint32_t reg, int *ok)
{
    io->w32(R8168_EPHYAR, (reg & 0x1f) << 16);
    if (!r8168_wait32(R8168_EPHYAR, R8168_ERIAR_FLAG, 1, 10, 100)) {
        *ok = 0;
        return 0;
    }
    *ok = 1;
    return (uint16_t)(io->r32(R8168_EPHYAR) & 0xffff);
}

/**
 * Write the EPHY word at `reg` (rtl_ephy_write).
 *
 * The trailing delay is the reference's and is not optional: consecutive EPHY
 * commands issued closer than 10 microseconds apart are not both seen.
 */
static int r8168_ephy_write(uint32_t reg, uint16_t value)
{
    int done;

    io->w32(R8168_EPHYAR, R8168_ERIAR_FLAG | ((reg & 0x1f) << 16) | (uint32_t)value);
    done = r8168_wait32(R8168_EPHYAR, R8168_ERIAR_FLAG, 0, 10, 100);
    io->udelay(10);
    return done;
}

/** Read the CSI dword at `addr` (rtl_csi_read); `*ok` reports timeout. */
static uint32_t r8168_csi_read(uint32_t addr, int *ok)
{
    io->w32(R8168_CSIAR,
            (addr & 0xfff) | R8168_CSIAR_BYTE_ENABLE | ((uint32_t)r8168_pci.function << 16));
    if (!r8168_wait32(R8168_CSIAR, R8168_CSIAR_FLAG, 1, 10, 100)) {
        *ok = 0;
        return 0;
    }
    *ok = 1;
    return io->r32(R8168_CSIDR);
}

/** Write the CSI dword at `addr` (rtl_csi_write). */
static int r8168_csi_write(uint32_t addr, uint32_t value)
{
    io->w32(R8168_CSIDR, value);
    io->w32(R8168_CSIAR, R8168_CSIAR_FLAG | (addr & 0xfff) | R8168_CSIAR_BYTE_ENABLE |
                             ((uint32_t)r8168_pci.function << 16));
    return r8168_wait32(R8168_CSIAR, R8168_CSIAR_FLAG, 0, 10, 100);
}

/* Bring-up. */

/** Unlock and lock the configuration registers (rtl_un/lock_config_regs). */
static void r8168_unlock_config(void)
{
    io->w8(R8168_CFG9346, R8168_CFG9346_UNLOCK);
}

static void r8168_lock_config(void)
{
    io->w8(R8168_CFG9346, R8168_CFG9346_LOCK);
}

/** Flush posted writes by reading a register back (rtl_pci_commit). */
static void r8168_pci_commit(void)
{
    (void)io->r8(R8168_CHIPCMD);
}

/** Mask and acknowledge every device interrupt (rtl8169_irq_mask_and_ack). */
static void r8168_irq_mask_and_ack(void)
{
    io->w16(R8168_INTRMASK, 0);
    io->w16(R8168_INTRSTATUS, 0xffff);
}

/**
 * Gate the receiver and wait for both FIFOs to drain (rtl_enable_rxdvgate).
 *
 * The two polls are the reference's rtl_wait_txrx_fifo_empty for this chip
 * version: the transmit FIFO reports empty in TxConfig, the receive side in
 * the MCU register. Both are bounded; a chip that never drains is reported,
 * not waited on forever.
 */
static int r8168_enable_rxdvgate(void)
{
    io->w32(R8168_MISC, io->r32(R8168_MISC) | R8168_MISC_RXDV_GATED);
    io->udelay(2000);
    if (!r8168_wait32(R8168_TXCONFIG, R8168_TXCFG_EMPTY, 1, 100, 42)) {
        return 0;
    }
    return r8168_wait8(R8168_MCU, R8168_MCU_RXTX_EMPTY, 1, 100, 42);
}

/** Ungate the receiver (rtl_disable_rxdvgate). RX is dead until this runs. */
static void r8168_disable_rxdvgate(void)
{
    io->w32(R8168_MISC, io->r32(R8168_MISC) & ~R8168_MISC_RXDV_GATED);
}

/** Reset the chip and wait for the bit to clear (rtl_hw_reset). */
static int r8168_hw_reset(void)
{
    io->w8(R8168_CHIPCMD, R8168_CMD_RESET);
    return r8168_wait8(R8168_CHIPCMD, R8168_CMD_RESET, 0, 100, 100);
}

/**
 * The pre-start initialization for this chip version (rtl_hw_init_8168g).
 *
 * The out-of-band bit and the two link-list handshakes are what make this
 * different from an ordinary reset: firmware, or the operating system that ran
 * before this one, can leave the MAC serving its management engine, and a
 * chip in that state ignores an ordinary bring-up. This is the sequence that
 * takes it back.
 */
static int r8168_hw_init_8168g(void)
{
    if (!r8168_enable_rxdvgate()) {
        r8168_err = "the FIFOs did not drain before reset";
        return 0;
    }

    io->w8(R8168_CHIPCMD,
           (uint8_t)(io->r8(R8168_CHIPCMD) & ~(R8168_CMD_TX_ENB | R8168_CMD_RX_ENB)));
    io->udelay(1000);
    io->w8(R8168_MCU, (uint8_t)(io->r8(R8168_MCU) & ~R8168_MCU_NOW_IS_OOB));

    r8168_mac_ocp_modify(0xe8de, 1u << 14, 0);
    if (!r8168_wait8(R8168_MCU, R8168_MCU_LINK_LIST_RDY, 1, 100, 42)) {
        r8168_err = "the link list did not become ready";
        return 0;
    }
    r8168_mac_ocp_modify(0xe8de, 0, 1u << 15);
    if (!r8168_wait8(R8168_MCU, R8168_MCU_LINK_LIST_RDY, 1, 100, 42)) {
        r8168_err = "the link list did not become ready after sharing";
        return 0;
    }
    return 1;
}

/**
 * Whether `mac` is a usable station address.
 *
 * The two shapes a stale or absent register reports are all zeroes and an
 * address with the multicast bit set; neither can be this machine's own.
 */
static int r8168_mac_valid(const uint8_t *mac)
{
    int i;
    int zero = 1;

    if (mac[0] & 0x01) {
        return 0;
    }
    for (i = 0; i < 6; i++) {
        if (mac[i] != 0) {
            zero = 0;
        }
    }
    return !zero;
}

/**
 * Read the station address the chip was built with.
 *
 * The reference takes the permanent address from the ERI window and uses the
 * MAC registers only as a fallback, and the order matters more here than it
 * does there: this driver runs after another operating system on a machine
 * that is not power-cycled between the two, so the register pair can hold
 * whatever that one left behind.
 *
 * @return 1 with `mac` filled, 0 when neither source gives a usable address.
 */
static int r8168_read_mac(uint8_t *mac)
{
    int ok = 0;
    uint32_t lo, hi;

    lo = r8168_eri_read(0xe0, &ok);
    if (ok) {
        hi = r8168_eri_read(0xe4, &ok);
        if (ok) {
            mac[0] = (uint8_t)(lo >> 0);
            mac[1] = (uint8_t)(lo >> 8);
            mac[2] = (uint8_t)(lo >> 16);
            mac[3] = (uint8_t)(lo >> 24);
            mac[4] = (uint8_t)(hi >> 0);
            mac[5] = (uint8_t)(hi >> 8);
            if (r8168_mac_valid(mac)) {
                return 1;
            }
        }
    }

    lo = io->r32(R8168_MAC0);
    hi = io->r32(R8168_MAC4);
    mac[0] = (uint8_t)(lo >> 0);
    mac[1] = (uint8_t)(lo >> 8);
    mac[2] = (uint8_t)(lo >> 16);
    mac[3] = (uint8_t)(lo >> 24);
    mac[4] = (uint8_t)(hi >> 0);
    mac[5] = (uint8_t)(hi >> 8);
    return r8168_mac_valid(mac);
}

/**
 * Turn PCIe ASPM, L0s and L1, off at both ends of the link this card is on.
 *
 * The reference asks the PCI layer to disable L1 and says why: on these parts
 * the link state can stop the device and hang the bus. A link has two ends,
 * and clearing the bit in the card alone leaves the state negotiated by the
 * port above it, so this also walks to the bridge whose secondary bus has
 * the card. The target machine's inventory reports this card running with
 * ASPM enabled under its previous operating system, so the bits really are
 * set when this kernel arrives.
 *
 * A card with no PCIe capability, or a bridge that cannot be found, is not a
 * failure: a device with no such control cannot be in the state this guards
 * against.
 */
static void r8168_disable_link_state(void)
{
    pci_device_t bridge;
    uint8_t cap;

    cap = pci_find_capability(r8168_pci.bus, r8168_pci.device, r8168_pci.function, PCI_CAP_ID_PCIE);
    if (cap != 0) {
        uint16_t ctl = pci_read_config16(r8168_pci.bus, r8168_pci.device, r8168_pci.function,
                                         (uint8_t)(cap + PCIE_LINK_CONTROL));

        pci_write_config16(r8168_pci.bus, r8168_pci.device, r8168_pci.function,
                           (uint8_t)(cap + PCIE_LINK_CONTROL),
                           (uint16_t)(ctl & ~PCIE_LINK_CTL_ASPM_MASK));
    }

    if (pci_find_bridge_for_bus(r8168_pci.bus, &bridge)) {
        cap = pci_find_capability(bridge.bus, bridge.device, bridge.function, PCI_CAP_ID_PCIE);
        if (cap != 0) {
            uint16_t ctl = pci_read_config16(bridge.bus, bridge.device, bridge.function,
                                             (uint8_t)(cap + PCIE_LINK_CONTROL));

            pci_write_config16(bridge.bus, bridge.device, bridge.function,
                               (uint8_t)(cap + PCIE_LINK_CONTROL),
                               (uint16_t)(ctl & ~PCIE_LINK_CTL_ASPM_MASK));
        }
    }
}

/** Program the station address (rtl_rar_set): two 32-bit writes, unlocked. */
static void r8168_rar_set(const uint8_t *mac)
{
    uint32_t low = (uint32_t)mac[0] | ((uint32_t)mac[1] << 8) | ((uint32_t)mac[2] << 16) |
                   ((uint32_t)mac[3] << 24);
    uint32_t high = (uint32_t)mac[4] | ((uint32_t)mac[5] << 8);

    r8168_unlock_config();
    io->w32(R8168_MAC4, high);
    r8168_pci_commit();
    io->w32(R8168_MAC0, low);
    r8168_pci_commit();
    r8168_lock_config();
}

/* The EPHY table for this chip version (e_info_8168g_2): register, the bits
 * cleared, the bits set, applied in this order. */
static const uint16_t r8168_ephy_table[][3] = {
    {0x00, 0x0008, 0x0000}, {0x0c, 0x3ff0, 0x0820}, {0x19, 0xffff, 0x7c00},
    {0x1e, 0xffff, 0x20eb}, {0x0d, 0xffff, 0x1666}, {0x00, 0xffff, 0x10a3},
    {0x06, 0xffff, 0xf050}, {0x04, 0x0000, 0x0010}, {0x1d, 0x4000, 0x0000}};

#define R8168_EPHY_ENTRIES ((int)(sizeof(r8168_ephy_table) / sizeof(r8168_ephy_table[0])))

/** Apply the EPHY table (rtl_ephy_init). */
static int r8168_ephy_init(void)
{
    int i;

    for (i = 0; i < R8168_EPHY_ENTRIES; i++) {
        int ok = 0;
        uint16_t v = r8168_ephy_read(r8168_ephy_table[i][0], &ok);

        if (!ok) {
            return 0;
        }
        v = (uint16_t)((v & ~r8168_ephy_table[i][1]) | r8168_ephy_table[i][2]);
        if (!r8168_ephy_write(r8168_ephy_table[i][0], v)) {
            return 0;
        }
    }
    return 1;
}

/**
 * The chip-specific configuration (rtl_hw_start_8168g).
 *
 * Twelve steps in the reference's order, and the order is the contract: the
 * packet filter is a clear THEN a set of one ERI bit, the two read-modify
 * writes of ERI 0x1b0 are separate because the reference writes the value
 * between them, and the receive gate is opened here, not later, because
 * the receiver is enabled after this returns.
 */
static int r8168_hw_start_8168g(void)
{
    int ok = 0;
    uint32_t v;

    /* FIFO sizes, static and dynamic (rtl_set_fifo_size
     * 0x08,0x10,0x02,0x06). */
    if (!r8168_eri_write(0xc8, R8168_ERIAR_MASK_1111, 0x00080002) ||
        !r8168_eri_write(0xe8, R8168_ERIAR_MASK_1111, 0x00100006)) {
        return 0;
    }
    /* Pause thresholds (rtl8168g_set_pause_thresholds 0x38, 0x48). */
    if (!r8168_eri_write(0xcc, R8168_ERIAR_MASK_0001, 0x38) ||
        !r8168_eri_write(0xd0, R8168_ERIAR_MASK_0001, 0x48)) {
        return 0;
    }
    /* ASPM entry latency (rtl_set_def_aspm_entry_latency): byte 0x27 into the
     * extended configuration space at 0x070f, reached through the chip's own
     * configuration window because this kernel addresses only the first 256
     * bytes of PCI configuration space. */
    v = r8168_csi_read(0x070c, &ok);
    if (!ok || !r8168_csi_write(0x070c, (v & ~0xff000000u) | 0x27000000u)) {
        return 0;
    }
    /* Packet filter reset (rtl_reset_packet_filter): clear then set. */
    if (!r8168_eri_rmw(0xdc, 1u, 0) || !r8168_eri_rmw(0xdc, 0, 1u)) {
        return 0;
    }
    if (!r8168_eri_write(0x2f8, R8168_ERIAR_MASK_0011, 0x1d8f)) {
        return 0;
    }
    r8168_disable_rxdvgate();
    if (!r8168_eri_write(0xc0, R8168_ERIAR_MASK_0011, 0x0000) ||
        !r8168_eri_write(0xb8, R8168_ERIAR_MASK_0011, 0x0000)) {
        return 0;
    }
    /* EEE on the MAC side (rtl8168_config_eee_mac). The reference configures
     * EEE instead of disabling it, and this port follows: leaving it alone
     * does not clear whatever state the previous operating system left. */
    io->w8(R8168_EEE_LED, (uint8_t)(io->r8(R8168_EEE_LED) & ~0x07));
    if (!r8168_eri_rmw(0x1b0, 0, 0x0003)) {
        return 0;
    }
    if (!r8168_eri_rmw(0x2fc, 0x06, 0x01)) {
        return 0;
    }
    if (!r8168_eri_rmw(0x1b0, 1u << 12, 0)) {
        return 0;
    }
    /* Leave the L2/L3 power state alone (rtl_pcie_state_l2l3_disable). */
    io->w8(R8168_CONFIG3, (uint8_t)(io->r8(R8168_CONFIG3) & ~0x02));
    return 1;
}

/** Set the receive filter (rtl_set_rx_mode): our address, broadcast and all
 *  multicast, which is what this stack's ARP and ICMP need. */
static void r8168_set_rx_mode(void)
{
    io->w32(R8168_MAR0 + 4, 0xffffffffu);
    io->w32(R8168_MAR0, 0xffffffffu);
    io->w32(R8168_RXCONFIG,
            (io->r32(R8168_RXCONFIG) & ~(uint32_t)R8168_RXCFG_ACCEPT_OK_MASK) | R8168_RXCFG_ACCEPT);
}

/**
 * Start the chip (rtl_hw_start), in the reference's order.
 *
 * The order is the whole of it: the configuration window opens, the chip's
 * own power management is turned off before the EPHY window is touched, the
 * chip-specific table runs, the ring addresses go in high half first, the
 * window closes, posted writes are flushed, and only then are the receiver
 * and transmitter enabled and the filters set. The interrupt mask is left
 * zero: `r8168_interrupts_enable` unmasks the sources once the card is up
 * and a handler serves its messages, and a card that sends none stays
 * polled.
 */
static int r8168_hw_start(void)
{
    r8168_unlock_config();

    /* Chip ASPM and clock request off before any EPHY access. */
    io->w8(R8168_CONFIG2, (uint8_t)(io->r8(R8168_CONFIG2) & ~0x80));
    io->w8(R8168_CONFIG5, (uint8_t)(io->r8(R8168_CONFIG5) & ~0x01));

    io->w16(R8168_CPLUSCMD, r8168_cp_cmd);

    /* The early-transmit threshold (rtl_hw_start_8168, 8168evl and later). */
    io->w8(R8168_MAXTXPACKETSIZE, 0x27);

    if (!r8168_hw_start_8168g() || !r8168_ephy_init()) {
        r8168_err = "the chip configuration sequence did not complete";
        return 0;
    }

    /* No interrupt coalescing. */
    io->w16(R8168_INTRMITIGATE, 0x0000);

    /* Allow the link to leave L1 (rtl_enable_exit_l1). */
    r8168_mac_ocp_modify(0xc0ac, 0x0000, 0x1f80);

    /* This chip version's ASPM is left off; the reference re-enables nothing
     * here either. */

    io->w16(R8168_RXMAXSIZE, R8168_RX_MAX_SIZE);

    /* Ring addresses, high half first, as the reference requires for the
     * transmit pair and does for both. */
    io->w32(R8168_TXDESC_HI, 0);
    io->w32(R8168_TXDESC_LO, (uint32_t)virt_to_phys(r8168_tx_ring));
    io->w32(R8168_RXDESC_HI, 0);
    io->w32(R8168_RXDESC_LO, (uint32_t)virt_to_phys(r8168_rx_ring));

    r8168_lock_config();
    r8168_pci_commit();

    io->w8(R8168_CHIPCMD, R8168_CMD_TX_ENB | R8168_CMD_RX_ENB);
    io->w32(R8168_RXCONFIG, R8168_RXCFG_VALUE);
    io->w32(R8168_TXCONFIG, R8168_TXCFG_VALUE);
    io->w32(R8168_RXCONFIG, io->r32(R8168_RXCONFIG) & ~(uint32_t)R8168_RXCFG_ACCEPT_ERR_MASK);
    r8168_set_rx_mode();

    io->w16(R8168_INTRMASK, 0x0000);
    return 1;
}

/* Rings. */

/** Hand every receive descriptor to the chip, the last one marked
 * end-of-ring. */
static void r8168_rx_fill(void)
{
    uint32_t i;

    for (i = 0; i < R8168_RING_SIZE; i++) {
        uint32_t eor = (i == R8168_RING_SIZE - 1) ? R8168_DESC_EOR : 0;

        r8168_rx_ring[i].addr_lo = (uint32_t)virt_to_phys(r8168_rx_buf[i]);
        r8168_rx_ring[i].addr_hi = 0;
        r8168_rx_ring[i].opts2 = 0;
        /* The address and options must be visible before ownership moves. */
        k_barrier();
        r8168_rx_ring[i].opts1 = R8168_DESC_OWN | eor | R8168_RX_BUF_SIZE;
    }
    r8168_rx_cur = 0;
}

/** Clear the transmit ring and its indices. */
static void r8168_tx_clear(void)
{
    uint32_t i;

    for (i = 0; i < R8168_RING_SIZE; i++) {
        r8168_tx_ring[i].opts1 = 0;
        r8168_tx_ring[i].opts2 = 0;
        r8168_tx_ring[i].addr_lo = 0;
        r8168_tx_ring[i].addr_hi = 0;
    }
    r8168_tx_head = 0;
    r8168_tx_tail = 0;
}

/* The PHY. */

/* MII registers this driver touches. */
#define R8168_MII_BMCR      0x00
#define R8168_MII_BMSR      0x01
#define R8168_MII_ADVERTISE 0x04
#define R8168_MII_CTRL1000  0x09

#define R8168_BMCR_RESET     0x8000
#define R8168_BMCR_ANENABLE  0x1000
#define R8168_BMCR_PDOWN     0x0800
#define R8168_BMCR_ANRESTART 0x0200

/* Everything this port advertises: 10 and 100 in both duplexes with the
 * 802.3 selector and pause in both directions (bits 10 and 11, which the
 * reference advertises through phylib), and 1000 full duplex. Pause is what
 * lets a switch hold this port instead of dropping a run of frames when the
 * far port is slower, as it is at 100 Mbit against this port's 1000. */
#define R8168_ADVERTISE_VALUE 0x0de1
#define R8168_CTRL1000_VALUE  0x0200

/**
 * Bring the PHY up and start autonegotiation.
 *
 * The reference reaches the PHY through phylib, which powers it up, applies a
 * firmware patch and configures EEE. This port clears the power-down bit the
 * reference's probe path leaves set, resets the PHY, sets the PHY-side EEE
 * bit (page 0x0a43, register 0x11, bit 4), and negotiates. That bit is the
 * whole of the reference's per-version PHY configuration apart from the
 * firmware patch, and it is applied because the MAC side is configured too:
 * the previous operating system leaves EEE enabled, and a PHY and a MAC that
 * disagree about it make a first link flap. The firmware patch is
 * deliberately absent; the reference proceeds without it too.
 */
static int r8168_phy_init(void)
{
    int ok = 0;
    uint16_t bmcr;

    r8168_ocp_base = R8168_OCP_STD_PHY_BASE;

    bmcr = r8168_mdio_read(R8168_MII_BMCR, &ok);
    if (!ok) {
        r8168_err = "the PHY did not answer";
        return 0;
    }
    /* Out of power-down, then a soft reset that the chip clears itself. */
    if (!r8168_mdio_write(R8168_MII_BMCR, (uint16_t)(bmcr & ~R8168_BMCR_PDOWN))) {
        r8168_err = "the PHY would not power up";
        return 0;
    }
    if (!r8168_mdio_write(R8168_MII_BMCR, R8168_BMCR_RESET)) {
        r8168_err = "the PHY would not reset";
        return 0;
    }
    {
        int i;

        for (i = 0; i < 100; i++) {
            bmcr = r8168_mdio_read(R8168_MII_BMCR, &ok);
            if (ok && !(bmcr & R8168_BMCR_RESET)) {
                break;
            }
            io->udelay(1000);
        }
        if (!ok || (bmcr & R8168_BMCR_RESET)) {
            r8168_err = "the PHY reset did not complete";
            return 0;
        }
    }

    /* The PHY-side EEE bit: page 0x0a43, register 0x11, bit 4. */
    r8168_mdio_page(0x0a43);
    {
        int page_ok = 0;
        uint16_t v = r8168_mdio_read(0x11, &page_ok);

        if (!page_ok || !r8168_mdio_write(0x11, (uint16_t)(v | (1u << 4)))) {
            r8168_err = "the PHY energy-efficiency bit would not set";
            r8168_mdio_page(0x0000);
            return 0;
        }
    }
    r8168_mdio_page(0x0000);

    if (!r8168_mdio_write(R8168_MII_ADVERTISE, R8168_ADVERTISE_VALUE) ||
        !r8168_mdio_write(R8168_MII_CTRL1000, R8168_CTRL1000_VALUE) ||
        !r8168_mdio_write(R8168_MII_BMCR, R8168_BMCR_ANENABLE | R8168_BMCR_ANRESTART)) {
        r8168_err = "autonegotiation would not start";
        return 0;
    }
    return 1;
}

/* PHYstatus bits (reference: enum rtl_register_content). */
#define R8168_PHY_TXFLOW  0x40
#define R8168_PHY_RXFLOW  0x20
#define R8168_PHY_LINK    0x02
#define R8168_PHY_1000F   0x10
#define R8168_PHY_100M    0x08
#define R8168_PHY_10M     0x04
#define R8168_PHY_FULLDUP 0x01

/** The link's speed in megabits as the PHYstatus sample `st` says it, or 0. */
static uint32_t r8168_speed_of(uint8_t st)
{
    if (!(st & R8168_PHY_LINK)) {
        return 0;
    }
    if (st & R8168_PHY_1000F) {
        return 1000;
    }
    if (st & R8168_PHY_100M) {
        return 100;
    }
    if (st & R8168_PHY_10M) {
        return 10;
    }
    return 0;
}

/* The driver's entry points. */

/* The card's own station address, read from the chip. */
static uint8_t rtl8168_mac[6];

/* The interface this driver's one card is. Kept from the bring-up, so a
 * frame it delivers says where it arrived. */
static struct netdev *rtl8168_iface;

/**
 * Find the chip, take it out of whatever state it was left in, and start it.
 *
 * The order is the reference's three stages: identify and quiesce, then the
 * rings and the PHY, then the start sequence. A failure at any point leaves
 * `r8168_err` naming it and the device unbound, and the probe then stops the
 * card. Link power management is turned off before anything else is
 * configured and before the rings exist, since it is the state the reference
 * warns can stop the device outright.
 *
 * @return `NETDEV_UP`, `NETDEV_ABSENT`, or `NETDEV_FAILED`.
 */
static int rtl8168_init(struct netdev *nd, int nth)
{
    uint32_t txconfig;
    uint32_t bar = 0;
    uint32_t addr;
    uintptr_t mapped;
    int i;

    nd->dev = 0; /* this driver keeps its one card in its own file */
    if (nth != 0) {
        return NETDEV_ABSENT;
    }
    rtl8168_iface = nd; /* only for the card this driver has */

    r8168_up = 0;
    if (!pci_find_device_by_id(0x10EC, 0x8168, &r8168_pci)) {
        r8168_err = "no RTL8168 present";
        return NETDEV_ABSENT;
    }
    nd->pci_key = r8168_pci.bus << 8 | r8168_pci.device << 3 | r8168_pci.function;

    /* The memory window, by index, as the reference takes it. */
    for (i = 0; i < 6; i++) {
        if (!pci_bar_is_memory(r8168_pci.bar[i])) {
            continue;
        }
        if (pci_bar_address(r8168_pci.bar[i]) != 0) {
            bar = r8168_pci.bar[i];
            break;
        }
        if (pci_bar_is_64bit(r8168_pci.bar[i])) {
            i++;
        }
    }
    if (bar == 0) {
        r8168_err = "the RTL8168 has no memory window";
        return NETDEV_FAILED;
    }
    addr = pci_bar_address(bar);
    mapped = map_mmio(addr, 256);
    if (mapped == 0) {
        r8168_err = "the RTL8168 window would not map";
        return NETDEV_FAILED;
    }
    r8168_mmio = mapped;
    pci_enable_memory_space(&r8168_pci);

    /* The chip's own revision, which is what decides whether this driver may
     * drive it: one PCI device id covers the whole 8168 family and its members
     * need different sequences. */
    txconfig = io->r32(R8168_TXCONFIG);
    if (txconfig == 0xffffffffu) {
        r8168_err = "the RTL8168 window reads back all ones";
        return NETDEV_FAILED;
    }
    r8168_xid = (txconfig >> R8168_XID_SHIFT) & R8168_XID_MASK;
    if ((r8168_xid & R8168_XID_FAMILY_MASK) != R8168_XID_8168GU) {
        r8168_err = "this RTL8168 is not the GU this driver was written for";
        return NETDEV_FAILED;
    }

    r8168_disable_link_state();

    r8168_cp_cmd = (uint16_t)(io->r16(R8168_CPLUSCMD) & R8168_CPCMD_MASK);
    io->w32(R8168_RXCONFIG, R8168_RXCFG_VALUE);
    r8168_irq_mask_and_ack();

    if (!r8168_hw_init_8168g()) {
        return NETDEV_FAILED;
    }
    if (!r8168_hw_reset()) {
        r8168_err = "the chip did not come out of reset";
        return NETDEV_FAILED;
    }

    if (!r8168_read_mac(rtl8168_mac)) {
        r8168_err = "the chip reports no usable station address";
        return NETDEV_FAILED;
    }

    r8168_tx_clear();
    r8168_rx_fill();
    pci_enable_bus_master(&r8168_pci);

    if (!r8168_phy_init()) {
        return NETDEV_FAILED;
    }

    /* The reference resets once more between the PHY work and the start. */
    r8168_irq_mask_and_ack();
    io->w32(R8168_RXCONFIG, io->r32(R8168_RXCONFIG) & ~(uint32_t)0x3f); /* rtl_rx_close */
    if (!r8168_enable_rxdvgate()) {
        r8168_err = "the FIFOs did not drain before the start";
        return NETDEV_FAILED;
    }
    io->udelay(2000);
    if (!r8168_hw_reset()) {
        r8168_err = "the chip did not come out of the second reset";
        return NETDEV_FAILED;
    }
    r8168_tx_clear();
    r8168_rx_fill();
    r8168_rar_set(rtl8168_mac);

    if (!r8168_hw_start()) {
        return NETDEV_FAILED;
    }

    r8168_up = 1;
    /* The tally outlives a warm restart (rtl8169_init_counter_offsets). */
    if (!r8168_counters_command(R8168_COUNTER_RESET)) {
        klog(KLOG_WARN, "rtl8168", "tally: not reset; its counts include earlier boots");
    }
    r8168_interrupts_enable();
    {
        int b;

        /* The card's address is its interface's; netdev_probe sets net_mac. */
        for (b = 0; b < 6; b++) {
            nd->mac[b] = rtl8168_mac[b];
        }
    }
    return NETDEV_UP;
}

/**
 * Reclaim finished transmit descriptors.
 *
 * Issue another transmit poll while descriptors remain outstanding, including
 * when this pass reclaimed none, to recover a lost poll request.
 */
static uint32_t r8168_tx_reclaim(void)
{
    uint32_t reclaimed = 0;

    while (r8168_tx_tail != r8168_tx_head) {
        uint32_t idx = r8168_tx_tail % R8168_RING_SIZE;

        if (r8168_tx_ring[idx].opts1 & R8168_DESC_OWN) {
            break;
        }
        /* The descriptor's other fields are read only after ownership is seen
         * to have returned. */
        k_barrier();
        r8168_tx_tail++;
        reclaimed++;
    }
    (void)reclaimed;
    if (r8168_tx_tail != r8168_tx_head) {
        io->w8(R8168_TXPOLL, R8168_TXPOLL_NPQ);
    }
    return reclaimed;
}

/**
 * Send one Ethernet frame.
 *
 * Fill, hand over, ring, and return: the chip finishes on its own time and the
 * service routine reclaims. Waiting here for completion would put a bounded
 * busy-wait inside every transmit, and every netconsole frame would run that
 * wait with interrupts disabled.
 *
 * A full ring is the one case that waits, briefly. Dropping the moment a
 * burst outruns the chip would lose frames a 10 ms tick would have freed room
 * for, so this rings the doorbell and reclaims for a bounded interval first,
 * and counts a stall only if the oldest descriptor is still the chip's after
 * that. A wedged chip therefore costs one bounded wait per frame, not a
 * kernel that stops.
 *
 * The ring work runs with interrupts disabled, as the RTL8139's transmit
 * does, because the service routine, from the tick or from the card's
 * message, reclaims from the same two ring indices this advances. `net.h`
 * already asks every syscall-context sender to hold `net_lock`; guarding
 * here as well means the interface's contract does not rest on each caller
 * remembering, and every driver behind that interface follows the same rule.
 * The bracket nests, so a caller that already has it pays nothing.
 */
static int rtl8168_tx(void *dev, const uint8_t *data, uint16_t len)
{
    uint32_t idx;
    uint32_t eor;
    uint32_t i;
    uint16_t total = len;
    uint32_t flags;

    (void)dev; /* this driver has one card */

    if (!r8168_up || len == 0 || len > R8168_FRAME_MAX) {
        return NETDEV_TX_DROPPED;
    }
    flags = net_lock();
    r8168_tx_reclaim();

    idx = r8168_tx_head % R8168_RING_SIZE;
    if (r8168_tx_ring[idx].opts1 & R8168_DESC_OWN) {
        for (i = 0; i < R8168_TX_WAIT_STEPS; i++) {
            io->w8(R8168_TXPOLL, R8168_TXPOLL_NPQ);
            io->udelay(R8168_TX_WAIT_USEC);
            r8168_tx_reclaim();
            if (!(r8168_tx_ring[idx].opts1 & R8168_DESC_OWN)) {
                break;
            }
        }
        if (r8168_tx_ring[idx].opts1 & R8168_DESC_OWN) {
            r8168_tx_stalls++;
            net_unlock(flags);
            return NETDEV_TX_DROPPED;
        }
    }

    k_memcpy(r8168_tx_buf[idx], data, len);
    /* The chip does not pad, and a short frame on the wire is a runt: pad to
     * the 60-byte minimum with zeroes so no stale buffer bytes are sent. */
    if (total < 60) {
        k_memset(r8168_tx_buf[idx] + total, 0, (size_t)(60 - total));
        total = 60;
    }

    eor = (idx == R8168_RING_SIZE - 1) ? R8168_DESC_EOR : 0;
    r8168_tx_ring[idx].addr_lo = (uint32_t)virt_to_phys(r8168_tx_buf[idx]);
    r8168_tx_ring[idx].addr_hi = 0;
    r8168_tx_ring[idx].opts2 = 0;
    /* Every field must be visible before ownership moves to the chip. */
    k_barrier();
    r8168_tx_ring[idx].opts1 =
        R8168_DESC_OWN | R8168_DESC_FS | R8168_DESC_LS | eor | (uint32_t)total;
    k_barrier();
    r8168_tx_head++;

    io->w8(R8168_TXPOLL, R8168_TXPOLL_NPQ);
    r8168_tx_packets++;
    net_unlock(flags);
    return NETDEV_TX_OK;
}

/**
 * Reclaim sent frames and take received ones off the ring and hand them up.
 *
 * This is the one service routine: the card's message handler runs it, and
 * so does the timer tick, which is the pump while the card sends no messages
 * and the fallback while it does. Bounded per call, because both callers run
 * with the rest of the machine waiting. Only whole single-descriptor frames
 * are delivered: a frame split across descriptors would need reassembly this
 * driver does not do, and at a 1500-byte transfer unit against a 16 KB
 * buffer the chip has no reason to split one. The reported length includes
 * the frame check sequence, which is stripped before delivery.
 *
 * @return the receive descriptors handled. Reclaimed transmits are not
 *         counted: a frame this driver sent, the log's own netconsole line
 *         among them, is no evidence that a source left set is one that
 *         clears.
 */
static uint32_t r8168_service(void)
{
    uint32_t budget;
    uint32_t work;

    if (!r8168_up) {
        return 0;
    }
    (void)r8168_tx_reclaim();
    work = 0;
    for (budget = 0; budget < R8168_RING_SIZE; budget++) {
        uint32_t idx = r8168_rx_cur % R8168_RING_SIZE;
        uint32_t opts1 = r8168_rx_ring[idx].opts1;
        uint32_t eor = (idx == R8168_RING_SIZE - 1) ? R8168_DESC_EOR : 0;
        uint32_t len;

        if (opts1 & R8168_DESC_OWN) {
            break;
        }
        /* The descriptor's payload is read only after ownership returned. */
        k_barrier();

        len = opts1 & R8168_DESC_LEN_MASK;
        if (opts1 & R8168_DESC_RX_RES) {
            r8168_rx_errors++;
            ethernet_note_rx_error(rtl8168_iface);
        } else if ((opts1 & (R8168_DESC_FS | R8168_DESC_LS)) != (R8168_DESC_FS | R8168_DESC_LS)) {
            r8168_rx_dropped++;
            ethernet_note_rx_drop(rtl8168_iface);
        } else if (len <= 4 || len > R8168_RX_BUF_SIZE) {
            r8168_rx_dropped++;
            ethernet_note_rx_drop(rtl8168_iface);
        } else {
            r8168_rx_packets++;
            ethernet_rx(rtl8168_iface, r8168_rx_buf[idx], (uint16_t)(len - 4));
        }
        work++;

        r8168_rx_ring[idx].opts2 = 0;
        k_barrier();
        r8168_rx_ring[idx].opts1 = R8168_DESC_OWN | eor | R8168_RX_BUF_SIZE;
        r8168_rx_cur++;
    }
    return work;
}

/**
 * The timer's pump: the service routine, every tick. With the card's
 * messages on this is the fallback, and what it finds is counted, since a
 * tick that does the work is the sign of a message that did not come or of
 * a handler that left work behind. While no handler acknowledges the status,
 * with no messages or after a storm, the tick notes and acknowledges it, so
 * a link change on a polled card is logged too.
 */
static void rtl8168_poll(void *dev)
{
    uint32_t work;

    (void)dev; /* this driver has one card */
    if (r8168_mmio != 0 && r8168_up && (r8168_msi_vector < 0 || r8168_msi_storms != 0)) {
        uint16_t status = io->r16(R8168_INTRSTATUS);

        if (status != 0) {
            r8168_note_sources(status);
            io->w16(R8168_INTRSTATUS, status);
        }
    }
    work = r8168_service();
    if (r8168_msi_vector >= 0) {
        r8168_tick_work += work;
    }
}

/*
 * Note what a status word says besides the rings, before the acknowledgement
 * clears it: the error sources are counted, since a frame the chip failed to
 * send has no other record, a descriptor coming back the same either way,
 * and a link change is logged. Every acknowledgement in service passes
 * through here: the handler's rounds, its last one for what arrived
 * meanwhile, and the tick's on a machine whose messages are off or masked.
 */
static void r8168_note_sources(uint16_t status)
{
    if (status & R8168_INTR_TXERR) {
        r8168_txerr_reports++;
    }
    if (status & R8168_INTR_RXERR) {
        r8168_rxerr_reports++;
    }
    if (status & R8168_INTR_LINKCHG) {
        r8168_log_link();
    }
}

/* How many times the handler services the card in one entry. Each round
 * may hand a ring's worth of frames up through the stack, so the bound is
 * what keeps the handler short; what is left goes to the tick or to the
 * next message. */
#define R8168_ISR_ROUNDS    2
/* How many entries in a row may find their status still set after the
 * acknowledgement, with nothing in the rings, before that is taken for a
 * storm. */
#define R8168_STORM_ENTRIES 16

/**
 * The card's message: acknowledge what it reports, then service the rings,
 * until the status reads clear or the bound is reached.
 *
 * The sources are masked for the length of this handler and unmasked at its
 * end, and that unmask is what arms the next message. A message is an edge:
 * the chip sends one when the sources it may report go from none set to one
 * set, and nothing while one stays set. Acknowledging the bits that were
 * read cannot be relied on to bring that condition down, because a frame
 * that arrives between the read and the write leaves a source set, and
 * because the chip reports states as well as events: transmit-descriptor
 * unavailable is true whenever the transmit ring is empty. After a handler
 * that returned with a source set the card would send no further message,
 * and the machine would take frames at the tick's pace from then on, which
 * is one receive window every ten milliseconds. Masking first makes the
 * condition false whatever the status holds, and the unmask makes it true
 * again for whatever arrived meanwhile, so no event is lost and none is
 * needed twice.
 *
 * The status is acknowledged before the rings are drained, as the xHCI
 * driver orders it: a frame that arrives while the rings are being drained
 * raises the bit again and the next round takes it, where a frame arriving
 * after an acknowledgement that followed the drain would wait for the tick.
 * A source the acknowledgement does not clear is read back still set at
 * once, with nothing in the rings through the whole entry;
 * R8168_STORM_ENTRIES entries in a row of that kind are a storm, and the
 * sources are left masked, counted, and the timer's pump drives the card
 * from then on. A bound reached with work still announced is not: the status
 * is acknowledged once more so that the next event raises a message, the
 * entry is counted as deferred, and the frames wait in the rings for the
 * tick or that message.
 */
static void rtl8168_isr(isr_params_t *params)
{
    uint32_t round;
    uint32_t work = 0;
    uint16_t status;
    int stuck = 0;

    (void)params;
    r8168_msi_entries++;
    io->w16(R8168_INTRMASK, 0);
    for (round = 0; round < R8168_ISR_ROUNDS; round++) {
        status = io->r16(R8168_INTRSTATUS);
        if (status == 0) {
            if (round == 0) {
                r8168_msi_idle++;
            }
            break;
        }
        io->w16(R8168_INTRSTATUS, status);
        stuck = (io->r16(R8168_INTRSTATUS) & status) == status;
        r8168_note_sources(status);
        work += r8168_service();
    }
    if (stuck && work == 0) {
        if (++r8168_msi_stuck >= R8168_STORM_ENTRIES) {
            r8168_msi_storms++;
            klog(KLOG_WARN, "rtl8168",
                 "interrupts: a source the acknowledgement does not clear; polled from now on");
            return; /* the sources stay masked; the tick drives the card */
        }
        io->w16(R8168_INTRMASK, R8168_INTR_ENABLED);
        return;
    }
    r8168_msi_stuck = 0;
    status = io->r16(R8168_INTRSTATUS);
    if (status != 0) {
        r8168_note_sources(status);
        io->w16(R8168_INTRSTATUS, status);
        r8168_msi_deferred++;
    }
    io->w16(R8168_INTRMASK, R8168_INTR_ENABLED);
}

/**
 * Take the card's messages, when the machine can route them.
 *
 * The handler is registered before the device's messages are enabled, so no
 * message can arrive on a vector nothing serves, and the sources are
 * unmasked last, once the device is known to send to this vector. A machine
 * with no usable APIC, no vector to give, or a capability that does not
 * enable stays polled, which the diagnostic line reports.
 */
static void r8168_interrupts_enable(void)
{
    int vector;

    r8168_msi_vector = -1;
    if (!isr_msi_ready()) {
        klog(KLOG_INFO, "rtl8168", "interrupts: polled, no usable APIC");
        return;
    }
    vector = isr_alloc_vector();
    if (vector < 0) {
        klog(KLOG_INFO, "rtl8168", "interrupts: polled, no vector free");
        return;
    }
    register_isr_handler((uint8_t)vector, rtl8168_isr, "rtl8168");
    if (pci_msi_enable(&r8168_pci, (uint8_t)vector) != 0) {
        klog(KLOG_INFO, "rtl8168", "interrupts: polled, the card sends no messages");
        return;
    }
    r8168_msi_vector = vector;
    io->w16(R8168_INTRSTATUS, 0xffff);
    io->w16(R8168_INTRMASK, R8168_INTR_ENABLED);
    klog(KLOG_INFO, "rtl8168", "interrupts: messages on vector %d", vector);
}

static const char *rtl8168_error(void)
{
    return r8168_err;
}

/**
 * The pause the link resolved, as PHYstatus reports it: whether this port
 * sends pause frames when its ring is full and whether it holds when the
 * far end sends them.
 */
static const char *r8168_pause_words(uint8_t st)
{
    if ((st & (R8168_PHY_TXFLOW | R8168_PHY_RXFLOW)) == (R8168_PHY_TXFLOW | R8168_PHY_RXFLOW)) {
        return "both ways";
    }
    if (st & R8168_PHY_TXFLOW) {
        return "sent only";
    }
    if (st & R8168_PHY_RXFLOW) {
        return "taken only";
    }
    return "none";
}

/**
 * Log the link as the MAC's summary has it: speed, duplex and pause, or
 * that it is down. Written at bring-up and at every link change the card
 * reports, so that a link that drops and returns, or one that came up at
 * a speed or without the pause expected, is in the log with its time.
 */
static void r8168_log_link(void)
{
    uint8_t st = io->r8(R8168_PHYSTATUS); /* one sample: every field of one state */
    uint32_t speed = r8168_speed_of(st);

    if (speed == 0) {
        klog(KLOG_INFO, "rtl8168", "link down");
        return;
    }
    klog(KLOG_INFO, "rtl8168", "link %d Mb/s, %s duplex, pause %s", (int)speed,
         (st & R8168_PHY_FULLDUP) ? "full" : "half", r8168_pause_words(st));
}

/**
 * Report the link, from the MAC's own summary of it.
 *
 * The speed comes from `r8168_speed_of` on one PHYstatus sample; a speed of
 * zero is what this chip says for no link.
 *
 * @return 1 with `*out` filled; 0 only when no window is mapped.
 */
static int rtl8168_link(void *dev, struct netdev_link *out)
{
    uint32_t speed;
    uint8_t st;

    (void)dev; /* this driver has one card */

    if (r8168_mmio == 0) {
        return 0;
    }
    st = io->r8(R8168_PHYSTATUS); /* one sample: every field of one state */
    speed = r8168_speed_of(st);
    out->up = speed != 0;
    out->mbps = (uint16_t)speed;
    out->full_duplex = (st & R8168_PHY_FULLDUP) ? 1 : 0;
    return 1;
}

/**
 * Give the chip a tally command (rtl8169_do_counters): `R8168_COUNTER_DUMP`
 * to write the counts into the block, or `R8168_COUNTER_RESET` to zero them.
 * The block's address goes into the two counter registers with the command
 * bit set, and the chip clears the bit once it has done the command.
 * Return zero if completion is not observed within the polling limit; read
 * the DMA block only after completion is observed. The chip is asked only
 * while it is up: the reference notes that some of these chips cannot dump
 * with the receiver stopped.
 *
 * The writes are in the reference's order: the high word, read back so the
 * write has landed, the low word as the address alone, then the address with
 * the command. In the reference's experience a chip given the command with
 * the address in the same write has dumped to the old address.
 */
static int r8168_counters_command(uint32_t command)
{
    uint32_t i;

    if (r8168_mmio == 0 || !r8168_up) {
        return 0;
    }
    io->w32(R8168_COUNTERADDRHIGH, 0);
    (void)io->r32(R8168_COUNTERADDRHIGH);
    io->w32(R8168_COUNTERADDRLOW, (uint32_t)virt_to_phys(&r8168_counters));
    io->w32(R8168_COUNTERADDRLOW, (uint32_t)virt_to_phys(&r8168_counters) | command);
    for (i = 0; i < R8168_COUNTERS_WAIT_STEPS; i++) {
        if (!(io->r32(R8168_COUNTERADDRLOW) & command)) {
            /* The block is read only after the chip says it wrote it. */
            k_barrier();
            return 1;
        }
        io->udelay(R8168_COUNTERS_WAIT_USEC);
    }
    return 0;
}

/**
 * The card's own counts for `/proc/net/dev`, from the tally block, in the
 * columns Linux's driver puts them: missed frames add to the receive drops,
 * alignment errors are the frame column, underruns the transmit fifo column,
 * one-collision and many-collision frames together the collisions, and the
 * transmissions the chip aborted the carrier column.
 */
static int rtl8168_counters(void *dev, struct netdev_counters *out)
{
    (void)dev; /* this driver has one card */

    if (!r8168_counters_command(R8168_COUNTER_DUMP)) {
        return 0;
    }
    out->rx_missed = r8168_counters.rx_missed;
    out->rx_frame = r8168_counters.align_errors;
    out->rx_multicast = r8168_counters.rx_multicast;
    out->tx_errors = r8168_counters.tx_errors_lo;
    out->tx_fifo = r8168_counters.tx_underrun;
    out->tx_collisions = r8168_counters.tx_one_collision + r8168_counters.tx_multi_collision;
    out->tx_carrier = r8168_counters.tx_aborted;
    return 1;
}

/**
 * The card's detail lines under the boot summary.
 *
 * The chip revision, the window and the address are a debug record, since
 * the station address is on the `net: eth0:` line above. The link follows,
 * then the counters once they count something, then how the card is served:
 * its messages and what the tick took, or the tick alone.
 */
static void rtl8168_print_info(void *dev)
{
    struct netdev_link link;

    (void)dev; /* this driver has one card */

    klog(KLOG_DEBUG, "rtl8168",
         "RTL8168GU: xid 0x%X, MMIO 0x%X, "
         "MAC %X%X:%X%X:%X%X:%X%X:%X%X:%X%X",
         r8168_xid, (unsigned)r8168_mmio, (unsigned)(rtl8168_mac[0] >> 4),
         (unsigned)(rtl8168_mac[0] & 0xF), (unsigned)(rtl8168_mac[1] >> 4),
         (unsigned)(rtl8168_mac[1] & 0xF), (unsigned)(rtl8168_mac[2] >> 4),
         (unsigned)(rtl8168_mac[2] & 0xF), (unsigned)(rtl8168_mac[3] >> 4),
         (unsigned)(rtl8168_mac[3] & 0xF), (unsigned)(rtl8168_mac[4] >> 4),
         (unsigned)(rtl8168_mac[4] & 0xF), (unsigned)(rtl8168_mac[5] >> 4),
         (unsigned)(rtl8168_mac[5] & 0xF));
    if (rtl8168_link(0, &link) && link.up) {
        r8168_log_link();
    } else {
        klog(KLOG_INFO, "rtl8168", "no link yet");
    }
    if (r8168_rx_packets || r8168_rx_errors || r8168_rx_dropped || r8168_rxerr_reports ||
        r8168_tx_packets || r8168_tx_stalls || r8168_txerr_reports) {
        klog(KLOG_INFO, "rtl8168",
             "rx %d (%d error, %d dropped, %d error reports), tx %d (%d stalled, %d error reports)",
             r8168_rx_packets, r8168_rx_errors, r8168_rx_dropped, r8168_rxerr_reports,
             r8168_tx_packets, r8168_tx_stalls, r8168_txerr_reports);
    }
    if (r8168_msi_vector >= 0 && r8168_msi_storms == 0 && r8168_msi_entries > 0) {
        klog(KLOG_INFO, "rtl8168",
             "messages on vector %d: %d, %d idle, %d deferred, %d by the tick", r8168_msi_vector,
             r8168_msi_entries, r8168_msi_idle, r8168_msi_deferred, r8168_tick_work);
    } else if (r8168_msi_vector >= 0 && r8168_msi_storms > 0) {
        klog(KLOG_INFO, "rtl8168",
             "messages on vector %d: %d, %d idle, %d deferred, masked after %d storm(s), %d by the "
             "tick",
             r8168_msi_vector, r8168_msi_entries, r8168_msi_idle, r8168_msi_deferred,
             r8168_msi_storms, r8168_tick_work);
    } else {
        klog(KLOG_INFO, "rtl8168", "polled by the tick");
    }
}

/**
 * Silence the chip: receive and transmit configuration cleared, the receive
 * gate closed, interrupts masked and messages off, DMA revoked.
 *
 * What the chip already accepted is first given a bounded moment to leave:
 * the shutdown path submits its last log line and then stops the device, and
 * a transmit returns once the descriptor is handed over, not once the frame
 * has gone. The gate is closed as well as the enables cleared because that is
 * the mechanism this chip has for stopping data at the receiver, and the
 * bring-up already uses it for the same purpose. Bus mastering goes last, for
 * the reason it goes last everywhere: it is the one that ends DMA whatever
 * state the chip is in, and the descriptor rings it has addresses for are
 * kernel memory.
 *
 * Safe on a chip that never finished starting, which is how the probe calls
 * it after a failed bring-up. `rtl8168_init` enables bus mastering and can
 * still fail afterwards in the PHY sequence or the hardware start, so this
 * runs against a half-configured chip and must not assume any of it.
 *
 * What it may touch depends on how far the probe got, and the revision check
 * is the line. One PCI device id covers the whole 8168 family, and the
 * registers this driver uses are the GU's: `MISC` at 0xf0 is the receive gate
 * on this member and something else on older ones. A chip whose revision this
 * driver refused therefore gets only what is safe on any of them, the
 * interrupt mask and the bus, and none of the sequence written for the GU.
 */
static void rtl8168_stop(void *dev)
{
    uint32_t i;

    (void)dev; /* this driver has one card */

    if (r8168_mmio != 0) {
        for (i = 0; i < R8168_DRAIN_STEPS; i++) {
            r8168_tx_reclaim();
            if (r8168_tx_tail == r8168_tx_head) {
                break;
            }
            io->w8(R8168_TXPOLL, R8168_TXPOLL_NPQ);
            io->udelay(R8168_DRAIN_USEC);
        }
        r8168_irq_mask_and_ack();
        if (r8168_msi_vector >= 0) {
            pci_msi_disable(&r8168_pci);
            r8168_msi_vector = -1;
        }
        if ((r8168_xid & R8168_XID_FAMILY_MASK) == R8168_XID_8168GU) {
            io->w8(R8168_CHIPCMD, 0);
            io->w32(R8168_RXCONFIG,
                    io->r32(R8168_RXCONFIG) & ~(uint32_t)R8168_RXCFG_ACCEPT_OK_MASK);
            (void)r8168_enable_rxdvgate();
        }
    }
    if (r8168_pci.vendor_id == 0x10EC && r8168_pci.device_id == 0x8168) {
        pci_disable_bus_master(&r8168_pci);
    }
    r8168_up = 0;
}

const struct netdev_ops rtl8168_netdev = {"RTL8168GU",   rtl8168_init,       rtl8168_tx,
                                          rtl8168_poll,  rtl8168_stop,       rtl8168_link,
                                          rtl8168_error, rtl8168_print_info, rtl8168_counters};
