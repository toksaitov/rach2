/*
 * e1000.c: the Intel 82540EM driver.
 *
 * The descriptor-ring design with nothing else on top. Two rings of 16-byte
 * descriptors live in kernel memory; the card owns a head index into each and
 * this driver owns a tail. To hand the card a receive buffer, advance the
 * receive tail; to send, fill a transmit descriptor and advance the transmit
 * tail. The card writes a done bit back into each descriptor it finishes.
 * That is the whole mechanism, and it is the one every gigabit card since has
 * elaborated on, which is why this driver is the one to read before
 * rtl8168.c.
 *
 * The card interrupts, so `poll` is NULL. It shares its PCI line, so the
 * handler services the card until its cause register reads clear, the same
 * rule rtl8139_irq follows and for the same reason.
 *
 * All register access goes through an indirection (`io`) so the host test can
 * drive the whole bring-up against a software model of the chip. The default
 * binding is plain MMIO on the mapped window. The card is emulated by QEMU,
 * so unlike the RTL8168GU this driver can also be run; the model exists to
 * reach the failures QEMU does not produce.
 */

#include "e1000.h"
#include "../net.h"
#include "../ethernet/ethernet.h"
#include "../../isr.h"
#include "../../pci.h"
#include "../../serial.h"
#include "../../udelay.h"
#include "../../mm/paging.h"

/* ------------------------------------------------------------------ */
/* Register access, behind an indirection the host test can replace.   */
/* ------------------------------------------------------------------ */

/**
 * The chip's registers and the short waits its handshakes need.
 *
 * Every register on this card is 32 bits wide, so unlike the RTL8168GU's
 * accessors there is one width here and no others are offered. `udelay` is a
 * floor: each use below is a poll interval, and waiting longer than asked is
 * always safe. The real binding is `k_udelay`, which measures against the
 * timer chip, because a counted loop cannot promise a duration on a processor
 * whose speed the kernel never learns.
 */
struct e1000_io {
    uint32_t (*r32)(uint32_t off);
    void (*w32)(uint32_t off, uint32_t value);
    void (*udelay)(uint32_t usec);
};

/* The mapped window; the real accessors read and write through it. */
static volatile uint8_t *e1000_mmio;

static uint32_t mmio_r32(uint32_t off)
{
    return *(volatile uint32_t *)(e1000_mmio + off);
}

static void mmio_w32(uint32_t off, uint32_t value)
{
    *(volatile uint32_t *)(e1000_mmio + off) = value;
}

static const struct e1000_io mmio_io = {mmio_r32, mmio_w32, k_udelay};

/* The binding in force. The host test points this at its model. */
static const struct e1000_io *io = &mmio_io;

/* ------------------------------------------------------------------ */
/* Driver state.                                                       */
/* ------------------------------------------------------------------ */

static pci_device_t e1000_pci;
static const char *e1000_err = "no e1000 present";
static int e1000_up;

/* What the boot detail line reports. The receive counters separate the four
 * reasons a frame is refused, because on this card they mean different
 * things: a hardware error, a frame the card split across descriptors, and a
 * length outside what a buffer can hold. */
static uint32_t e1000_rx_packets;
static uint32_t e1000_rx_errors;
static uint32_t e1000_rx_dropped;
static uint32_t e1000_tx_packets;
static uint32_t e1000_tx_stalls;

/*
 * The rings and their buffers, in .bss, which this kernel places below the
 * 32 MiB identity-mapped limit: the virtual address is the physical one, so
 * what goes into a descriptor is the address of the array. The manual asks
 * for 16-byte alignment on each ring.
 *
 * The rings are volatile and the buffers are not, which is the distinction
 * between memory the card writes while this driver is reading it and memory
 * only one of them touches at a time. A done bit the compiler kept in a
 * register would be a wait that never ends. The kernel builds this file
 * without optimization, where that cannot happen, but the host model builds
 * the same file at -O1 and -O2, so the type says what is true rather than
 * resting on a flag. A buffer is read only after its descriptor reported
 * done, so making 112 KiB of buffer volatile would slow every copy to no
 * purpose.
 */
static volatile struct e1000_rx_desc e1000_rx_ring[E1000_RING_SIZE] __attribute__((aligned(16)));
static volatile struct e1000_tx_desc e1000_tx_ring[E1000_RING_SIZE] __attribute__((aligned(16)));
static uint8_t e1000_rx_buf[E1000_RING_SIZE][E1000_RX_BUF_SIZE] __attribute__((aligned(8)));
static uint8_t e1000_tx_buf[E1000_RING_SIZE][E1000_TX_BUF_SIZE] __attribute__((aligned(8)));

static uint32_t e1000_rx_cur;  /* next receive descriptor to inspect */
static uint32_t e1000_tx_head; /* next transmit descriptor to fill */

static void e1000_irq(isr_params_t *params);
static void e1000_rx(void);
static int e1000_tx_frame(const uint8_t *data, uint16_t len);

/* ------------------------------------------------------------------ */
/* Bring-up.                                                           */
/* ------------------------------------------------------------------ */

/**
 * Reset the device and wait for the handshake to complete.
 *
 * `CTRL.RST` is self-clearing: the card raises it and drops it again when the
 * reset is done. Interrupts are masked before the reset and again after it,
 * because a reset restores the mask to its power-on value and an unmasked
 * cause with no handler registered would be an interrupt nobody answers.
 *
 * @return 1 when the card came out of reset, 0 when the budget ran out.
 */
static int e1000_reset(void)
{
    uint32_t i;

    io->w32(E1000_IMC, 0xffffffffu);
    (void)io->r32(E1000_ICR);

    io->w32(E1000_CTRL, io->r32(E1000_CTRL) | E1000_CTRL_RST);

    for (i = 0; i < E1000_RESET_STEPS; i++) {
        io->udelay(E1000_RESET_USEC);
        if ((io->r32(E1000_CTRL) & E1000_CTRL_RST) == 0) {
            io->w32(E1000_IMC, 0xffffffffu);
            (void)io->r32(E1000_ICR);
            return 1;
        }
    }
    return 0;
}

/**
 * Read one 16-bit EEPROM word through `EERD`.
 *
 * The address goes in with the start bit and the card answers by setting
 * done and placing the word in the high half. The field positions are the
 * 82540EM's; the 82544 and later parts moved both the address field and the
 * done bit, so a card that never reports done is one this driver should not
 * be reading, and the caller treats a 0 return as that rather than retrying.
 *
 * @return 1 on success with `*out` set, 0 when the card did not answer.
 */
static int e1000_eeprom_read(uint16_t word, uint16_t *out)
{
    uint32_t value;
    uint32_t i;

    io->w32(E1000_EERD, E1000_EERD_START | ((uint32_t)word << E1000_EERD_ADDR_SHIFT));

    for (i = 0; i < E1000_EEPROM_STEPS; i++) {
        value = io->r32(E1000_EERD);
        if (value & E1000_EERD_DONE) {
            *out = (uint16_t)(value >> E1000_EERD_DATA_SHIFT);
            return 1;
        }
        io->udelay(E1000_EEPROM_USEC);
    }
    return 0;
}

/**
 * Reject a station address no card may transmit from.
 *
 * All zeroes is a card that has not loaded one. A group address, which is
 * every address with the low bit of the first byte set, cannot be a station
 * address; the broadcast address is one of those, so it needs no separate
 * test.
 */
static int e1000_mac_valid(const uint8_t *mac)
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
 * Latch the station address into `mac`.
 *
 * `RAL0` and `RAH0` are tried first: a card that loaded its address from its
 * own EEPROM at power-on has it there already, with the valid bit set, and
 * this is the path a QEMU machine takes. The EEPROM is read only when that
 * bit is clear, which is what a card presents when firmware has not touched
 * it. Both paths exist because a driver that has only the first would work on
 * every emulator and fail on a cold real card.
 *
 * @return 1 with `mac` filled, 0 when neither source gave a usable address.
 */
static int e1000_read_mac(uint8_t *mac)
{
    uint32_t low;
    uint32_t high;
    uint16_t word;
    int i;

    high = io->r32(E1000_RAH0);
    if (high & E1000_RAH_AV) {
        low = io->r32(E1000_RAL0);
        mac[0] = (uint8_t)(low >> 0);
        mac[1] = (uint8_t)(low >> 8);
        mac[2] = (uint8_t)(low >> 16);
        mac[3] = (uint8_t)(low >> 24);
        mac[4] = (uint8_t)(high >> 0);
        mac[5] = (uint8_t)(high >> 8);
        if (e1000_mac_valid(mac)) {
            return 1;
        }
    }

    for (i = 0; i < 3; i++) {
        if (!e1000_eeprom_read((uint16_t)i, &word)) {
            return 0;
        }
        mac[i * 2] = (uint8_t)(word & 0xff);
        mac[i * 2 + 1] = (uint8_t)(word >> 8);
    }
    return e1000_mac_valid(mac);
}

/**
 * Publish the station address so the card filters unicast to it.
 *
 * Written whichever source it came from: an address read out of the EEPROM
 * has to be put into `RAL0` and `RAH0` before the card will accept a frame
 * addressed to it, and rewriting one that was already there costs nothing.
 */
static void e1000_set_mac(const uint8_t *mac)
{
    uint32_t low = (uint32_t)mac[0] | ((uint32_t)mac[1] << 8) | ((uint32_t)mac[2] << 16) |
                   ((uint32_t)mac[3] << 24);
    uint32_t high = (uint32_t)mac[4] | ((uint32_t)mac[5] << 8) | E1000_RAH_AV;

    io->w32(E1000_RAL0, low);
    io->w32(E1000_RAH0, high);
}

/**
 * Program the receive ring and hand every descriptor but one to the card.
 *
 * The ring is empty when head and tail are equal, so one descriptor is always
 * left out of the card's reach; without that gap a full ring would be
 * indistinguishable from an empty one. The base, length, and both indices are
 * set before `RCTL.EN`, which is what the caller enables afterwards: a
 * receiver started against an unprogrammed ring would fetch descriptors from
 * address zero.
 */
static void e1000_rx_setup(void)
{
    uint32_t i;

    for (i = 0; i < E1000_RING_SIZE; i++) {
        e1000_rx_ring[i].addr_lo = (uint32_t)(uintptr_t)e1000_rx_buf[i];
        e1000_rx_ring[i].addr_hi = 0;
        e1000_rx_ring[i].len_csum = 0;
        e1000_rx_ring[i].sta_err = 0;
    }
    e1000_rx_cur = 0;

    io->w32(E1000_RDBAL, (uint32_t)(uintptr_t)e1000_rx_ring);
    io->w32(E1000_RDBAH, 0);
    io->w32(E1000_RDLEN, E1000_RING_BYTES);
    io->w32(E1000_RDH, 0);
    io->w32(E1000_RDT, E1000_RING_SIZE - 1);
}

/**
 * Program the transmit ring with every descriptor marked done.
 *
 * A descriptor is free when its status carries `DD`, which is the bit the
 * card sets when it finishes one. A descriptor that has never been used
 * carries zero, and would read as busy forever, so the ring starts with the
 * bit set on every entry and the transmit path clears it as it hands each one
 * over. One bit then answers the question for a fresh ring and a running one
 * alike.
 */
static void e1000_tx_setup(void)
{
    uint32_t i;

    for (i = 0; i < E1000_RING_SIZE; i++) {
        e1000_tx_ring[i].addr_lo = 0;
        e1000_tx_ring[i].addr_hi = 0;
        e1000_tx_ring[i].lower = 0;
        e1000_tx_ring[i].upper = E1000_TXD_STAT_DD;
    }
    e1000_tx_head = 0;

    io->w32(E1000_TDBAL, (uint32_t)(uintptr_t)e1000_tx_ring);
    io->w32(E1000_TDBAH, 0);
    io->w32(E1000_TDLEN, E1000_RING_BYTES);
    io->w32(E1000_TDH, 0);
    io->w32(E1000_TDT, 0);
    io->w32(E1000_TIPG, E1000_TIPG_IPGT | (E1000_TIPG_IPGR1 << E1000_TIPG_IPGR1_SHIFT) |
                            (E1000_TIPG_IPGR2 << E1000_TIPG_IPGR2_SHIFT));
}

/**
 * Probe the card, reset it, program both rings, and start it.
 *
 * The order is the one the manual asks for and the one the host model
 * asserts: the window is mapped and answering before anything is written to
 * it, interrupts are masked before the reset, the station address and the
 * multicast table are set before the receiver could match a frame, both rings
 * are complete before bus mastering is enabled, and the receiver and
 * transmitter are the last two things switched on. Nothing before the PCI
 * lookup succeeds touches a register, which is what the driver table requires
 * of a card that is not in the machine.
 *
 * @return `NETDEV_UP`, `NETDEV_ABSENT` when there is no such card, or
 *         `NETDEV_FAILED` with `e1000_error()` set to the reason.
 */
static int e1000_init(void)
{
    uint32_t addr;
    uint32_t bar = 0;
    uint32_t ctrl;
    uint32_t i;

    e1000_up = 0;
    if (!pci_find_device_by_id(E1000_VENDOR, E1000_DEVICE, &e1000_pci)) {
        e1000_err = "no e1000 present";
        return NETDEV_ABSENT;
    }

    /* Reject an unrouted interrupt line rather than register a vector the
     * card will never raise; this driver has no polled path to fall back
     * to. */
    if (e1000_pci.irq == 0xFF) {
        e1000_err = "the e1000 has no usable IRQ line";
        return NETDEV_FAILED;
    }

    for (i = 0; i < 6; i++) {
        if (!pci_bar_is_memory(e1000_pci.bar[i])) {
            continue;
        }
        if (pci_bar_address(e1000_pci.bar[i]) != 0) {
            bar = e1000_pci.bar[i];
            break;
        }
        if (pci_bar_is_64bit(e1000_pci.bar[i])) {
            i++;
        }
    }
    if (bar == 0) {
        e1000_err = "the e1000 has no memory window";
        return NETDEV_FAILED;
    }
    addr = pci_bar_address(bar);
    if (map_mmio(addr, E1000_BAR_WINDOW) == 0) {
        e1000_err = "the e1000 window would not map";
        return NETDEV_FAILED;
    }
    e1000_mmio = (volatile uint8_t *)(uintptr_t)addr;
    pci_enable_memory_space(&e1000_pci);

    /* A window that answers every read with all ones is a card that is not
     * decoding it, and every check after this one would read the same. */
    if (io->r32(E1000_STATUS) == 0xffffffffu) {
        e1000_err = "the e1000 window reads back all ones";
        return NETDEV_FAILED;
    }

    if (!e1000_reset()) {
        e1000_err = "the e1000 did not come out of reset";
        return NETDEV_FAILED;
    }

    if (!e1000_read_mac(net_mac)) {
        e1000_err = "the e1000 reports no usable station address";
        return NETDEV_FAILED;
    }

    /* Ask for the link and let the card negotiate its own speed and duplex.
     * VLAN tagging is cleared because nothing above this layer reads a tag,
     * and a card left in VLAN mode would strip four bytes the stack expects
     * to see. */
    ctrl = io->r32(E1000_CTRL);
    ctrl |= E1000_CTRL_SLU | E1000_CTRL_ASDE;
    ctrl &= ~(E1000_CTRL_VME | E1000_CTRL_PHY_RST);
    io->w32(E1000_CTRL, ctrl);

    /* An unwritten multicast table is not defined after a reset, and a stale
     * entry would admit traffic this machine did not ask for. */
    for (i = 0; i < E1000_MTA_ENTRIES; i++) {
        io->w32(E1000_MTA + i * 4, 0);
    }
    e1000_set_mac(net_mac);

    e1000_rx_setup();
    e1000_tx_setup();

    /* Nothing above could have caused a transfer: the card fetches a
     * descriptor only once the receiver or transmitter is enabled, and both
     * are still off. */
    pci_enable_bus_master(&e1000_pci);

    register_isr_handler((uint8_t)(32 + e1000_pci.irq), e1000_irq);
    io->w32(E1000_IMS, E1000_INT_MASK);

    io->w32(E1000_RCTL, E1000_RCTL_EN | E1000_RCTL_BAM | E1000_RCTL_SZ_2048 | E1000_RCTL_SECRC);
    io->w32(E1000_TCTL, E1000_TCTL_EN | E1000_TCTL_PSP |
                            ((uint32_t)E1000_TCTL_CT_DEFAULT << E1000_TCTL_CT_SHIFT) |
                            ((uint32_t)E1000_TCTL_COLD_FD << E1000_TCTL_COLD_SHIFT));

    e1000_up = 1;
    return NETDEV_UP;
}

/* ------------------------------------------------------------------ */
/* Transmit.                                                           */
/* ------------------------------------------------------------------ */

/**
 * Copy a frame into the next transmit descriptor and hand it to the card.
 *
 * A full ring waits, briefly. Dropping the moment a burst outruns the card
 * would lose frames that a millisecond would have made room for, so this
 * polls the descriptor's done bit for a bounded interval and counts a stall
 * only if it is still the card's after that; a wedged card therefore costs
 * one bounded wait per frame rather than a kernel that stops.
 *
 * Short frames are padded to the 60-byte Ethernet minimum with zeroes, so no
 * stale buffer bytes reach the wire. `TCTL.PSP` asks the card to do the same,
 * and both are set because an emulated card may implement one and not the
 * other, and a runt on the wire is the failure that would follow.
 *
 * The whole of it runs under `net_lock`, as `rtl8168_tx` does: the ring index
 * this advances is also read by the interrupt handler's path back into the
 * stack, and `net.h` asks every syscall-context sender to hold the lock
 * anyway. The bracket nests, so a caller that already holds it pays nothing.
 */
static int e1000_tx_frame(const uint8_t *data, uint16_t len)
{
    uint32_t idx;
    uint32_t i;
    uint16_t total = len;
    uint32_t flags;

    if (!e1000_up || len == 0 || len > E1000_FRAME_MAX) {
        if (len > E1000_FRAME_MAX) {
            serial_printf("e1000: tx: frame of %d bytes is too large, dropped\n", (int)len);
        }
        return NETDEV_TX_DROPPED;
    }

    flags = net_lock();
    idx = e1000_tx_head % E1000_RING_SIZE;

    if (!(E1000_TXD_STATUS(&e1000_tx_ring[idx]) & E1000_TXD_STAT_DD)) {
        for (i = 0; i < E1000_TX_WAIT_STEPS; i++) {
            io->udelay(E1000_TX_WAIT_USEC);
            if (E1000_TXD_STATUS(&e1000_tx_ring[idx]) & E1000_TXD_STAT_DD) {
                break;
            }
        }
        if (!(E1000_TXD_STATUS(&e1000_tx_ring[idx]) & E1000_TXD_STAT_DD)) {
            e1000_tx_stalls++;
            net_unlock(flags);
            return NETDEV_TX_DROPPED;
        }
    }

    for (i = 0; i < len; i++) {
        e1000_tx_buf[idx][i] = data[i];
    }
    /* Pad to the Ethernet minimum with zeroes, so no stale buffer bytes reach
     * the wire. Written as a loop, as rtl8168.c writes its own: this file is
     * compiled natively by its host model as well as freestanding by the
     * kernel, and it reaches for nothing outside itself. */
    if (total < 60) {
        for (i = total; i < 60; i++) {
            e1000_tx_buf[idx][i] = 0;
        }
        total = 60;
    }

    e1000_tx_ring[idx].addr_lo = (uint32_t)(uintptr_t)e1000_tx_buf[idx];
    e1000_tx_ring[idx].addr_hi = 0;
    e1000_tx_ring[idx].upper = 0; /* clear DD; the card sets it when done */
    e1000_tx_ring[idx].lower = E1000_TXD_LOWER(
        total, E1000_TXD_CMD_EOP | E1000_TXD_CMD_IFCS | E1000_TXD_CMD_RS);

    /* Every field of the descriptor must be visible before the tail moves,
     * because the tail is what tells the card to read it. */
    __asm__ __volatile__("" : : : "memory");
    e1000_tx_head++;
    io->w32(E1000_TDT, e1000_tx_head % E1000_RING_SIZE);

    e1000_tx_packets++;
    net_unlock(flags);
    return NETDEV_TX_OK;
}

/* ------------------------------------------------------------------ */
/* Receive.                                                            */
/* ------------------------------------------------------------------ */

/**
 * Take finished frames off the receive ring and hand them up.
 *
 * Four things are checked before a frame is delivered, and any one of them
 * failing drops it and counts it. The error byte must be clear, because a
 * descriptor can report done and end-of-packet and still carry a receive
 * error. `EOP` must be set on the same descriptor, because a frame split
 * across two would need a reassembly this driver does not do, and at a
 * 1500-byte transfer unit against a 2048-byte buffer the card has no reason
 * to split one. The length must cover an Ethernet header, and it must not
 * exceed the buffer, because a length taken on trust would hand the stack
 * bytes past the end of the array.
 *
 * `SECRC` is set, so the length is the frame's own and the frame check
 * sequence has already been removed. Each descriptor's status is cleared
 * before it goes back to the card, and the tail is written once at the end
 * rather than per descriptor: one register write returns the whole batch.
 */
static void e1000_rx(void)
{
    uint32_t taken = 0;
    uint32_t last = 0;
    uint32_t budget;

    for (budget = 0; budget < E1000_RING_SIZE; budget++) {
        volatile struct e1000_rx_desc *d = &e1000_rx_ring[e1000_rx_cur];
        uint8_t status = E1000_RXD_STATUS(d);
        uint8_t errors;
        uint32_t len;

        if (!(status & E1000_RXD_STAT_DD)) {
            break;
        }
        /* The descriptor's payload is read only after done was seen. */
        __asm__ __volatile__("" : : : "memory");

        errors = E1000_RXD_ERRORS(d);
        len = E1000_RXD_LEN(d);

        if (errors & E1000_RXD_ERR_ANY) {
            e1000_rx_errors++;
            ethernet_note_rx_error();
        } else if (!(status & E1000_RXD_STAT_EOP)) {
            e1000_rx_dropped++;
            ethernet_note_rx_drop();
        } else if (len < E1000_FRAME_MIN || len > E1000_RX_BUF_SIZE) {
            e1000_rx_dropped++;
            ethernet_note_rx_drop();
        } else {
            e1000_rx_packets++;
            ethernet_rx(e1000_rx_buf[e1000_rx_cur], (uint16_t)len);
        }

        d->len_csum = 0;
        d->sta_err = 0;
        __asm__ __volatile__("" : : : "memory");

        last = e1000_rx_cur;
        taken++;
        e1000_rx_cur = (e1000_rx_cur + 1) % E1000_RING_SIZE;
    }

    if (taken != 0) {
        io->w32(E1000_RDT, last);
    }
}

/**
 * Service the card until its cause register reads clear.
 *
 * The card shares a PCI interrupt line, and a frame arriving after the cause
 * was read but before the ring drained would raise it again. Reading in a
 * loop until nothing is pending leaves the line deasserted and the ring
 * drained on return, so no arrival is stranded until the next interrupt. The
 * guard bounds the loop should a cause ever stick. Reading `ICR` is what
 * clears it; the shared interrupt router has already sent the PIC its end of
 * interrupt, so this handler must not touch the PIC.
 */
static void e1000_irq(isr_params_t *params)
{
    uint32_t cause;
    int guard;

    (void)params;

    for (guard = 0; guard < 64; guard++) {
        cause = io->r32(E1000_ICR);
        if ((cause & E1000_INT_MASK) == 0) {
            break;
        }
        if (cause & (E1000_INT_RXT0 | E1000_INT_RXDMT0)) {
            e1000_rx();
        }
        if (cause & E1000_INT_RXO) {
            /* The ring filled while the machine was elsewhere. Draining it is
             * the whole recovery: the descriptors the card could not use are
             * returned by the tail write e1000_rx ends with. */
            e1000_rx_dropped++;
            ethernet_note_rx_drop();
            e1000_rx();
        }
        /* A link change needs nothing done. The card renegotiates on its own
         * and print_info reads the current state when it is asked. */
    }
}

/* ------------------------------------------------------------------ */
/* What the layers above ask for.                                      */
/* ------------------------------------------------------------------ */

static const char *e1000_error(void)
{
    return e1000_err;
}

/**
 * Silence the card: receiver and transmitter off, interrupts masked, DMA
 * revoked.
 *
 * The order is what makes it safe rather than merely tidy. The enables go
 * first, so the card stops fetching descriptors while it still has a window
 * to be told through; the mask follows, so nothing raises a line whose
 * handler is about to be irrelevant; bus mastering goes last, because it is
 * the one that ends DMA whatever the card believes, and clearing it before
 * the card was told to stop would leave a device still trying.
 *
 * Safe on a card that never finished starting. `e1000_init` enables bus
 * mastering before it enables the receiver, and can still fail after that, so
 * the probe calls this on a driver that reported `NETDEV_FAILED` and the
 * window may hold a half-configured card. The PCI record is written only when
 * a device was found, which `e1000_up` does not tell us; the window pointer
 * does, because nothing is written to a card whose window never mapped.
 */
static void e1000_stop(void)
{
    if (e1000_mmio != 0) {
        io->w32(E1000_RCTL, 0);
        io->w32(E1000_TCTL, 0);
        io->w32(E1000_IMC, 0xffffffffu);
        (void)io->r32(E1000_ICR);
    }
    if (e1000_pci.vendor_id == E1000_VENDOR && e1000_pci.device_id == E1000_DEVICE) {
        pci_disable_bus_master(&e1000_pci);
    }
    e1000_up = 0;
}

/**
 * The card's detail lines under the boot summary.
 *
 * `serial_printf` has no field width, so the station address is printed a
 * nibble at a time, as the other two drivers print theirs.
 */
static void e1000_print_info(void)
{
    uint32_t status = io->r32(E1000_STATUS);
    uint32_t speed = (status & E1000_STATUS_SPEED_MASK) >> E1000_STATUS_SPEED_SHIFT;

    serial_printf("    e1000: 82540EM, MMIO 0x%X, IRQ %d, "
                  "MAC %X%X:%X%X:%X%X:%X%X:%X%X:%X%X\n",
                  (unsigned)(uintptr_t)e1000_mmio, (int)e1000_pci.irq,
                  (unsigned)(net_mac[0] >> 4), (unsigned)(net_mac[0] & 0xF),
                  (unsigned)(net_mac[1] >> 4), (unsigned)(net_mac[1] & 0xF),
                  (unsigned)(net_mac[2] >> 4), (unsigned)(net_mac[2] & 0xF),
                  (unsigned)(net_mac[3] >> 4), (unsigned)(net_mac[3] & 0xF),
                  (unsigned)(net_mac[4] >> 4), (unsigned)(net_mac[4] & 0xF),
                  (unsigned)(net_mac[5] >> 4), (unsigned)(net_mac[5] & 0xF));
    if (!(status & E1000_STATUS_LU)) {
        serial_printf("    e1000: no link yet\n");
    } else {
        serial_printf("    e1000: link %d Mb/s, %s duplex\n",
                      speed == 0 ? 10 : (speed == 1 ? 100 : 1000),
                      (status & E1000_STATUS_FD) ? "full" : "half");
    }
    serial_printf("    e1000: rx %d (%d error, %d dropped), tx %d (%d stalled)\n",
                  e1000_rx_packets, e1000_rx_errors, e1000_rx_dropped, e1000_tx_packets,
                  e1000_tx_stalls);
}

/* The card's entry in the driver table. `poll` is NULL: this one
 * interrupts. */
const struct netdev_ops e1000_netdev = {"e1000",     e1000_init,  e1000_tx_frame, 0,
                                        e1000_stop,  e1000_error, e1000_print_info};
