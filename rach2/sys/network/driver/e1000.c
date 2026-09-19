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

#include "../../klog.h"
#include "../../lib/kstring.h"
#include "e1000.h"
#include "../net.h"
#include "../ethernet/ethernet.h"
#include "../../isr.h"
#include "../../kio.h"
#include "../../mm/paging.h"
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
struct e1000;

struct e1000_io {
    uint32_t (*r32)(struct e1000 *e, uint32_t off);
    void (*w32)(struct e1000 *e, uint32_t off, uint32_t value);
    /* A write through the card's I/O window rather than its memory window.
       Only the reset uses it; see the note on E1000_IO_ADDR. */
    void (*wio)(struct e1000 *e, uint32_t off, uint32_t value);
    void (*udelay)(uint32_t usec);
};

/* How many of these cards this kernel drives. A machine with two of them has
 * two records; each has its own registers, rings and counters, and nothing
 * one of them does reaches the other. */
#define E1000_MAX 4

/*
 * One card. The rings and their buffers are members, so they are in .bss,
 * which this kernel places below the 32 MiB identity-mapped limit: the
 * virtual address is the physical one, so what goes into a descriptor is the
 * address of the array. The manual asks for 16-byte alignment on each ring.
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
struct e1000 {
    int used;
    int number; /* its place in the table, and what the log names it by */
    /* The interface this card is, so a frame it delivers says where it
     * arrived and its counters are that interface's. */
    struct netdev *nd;

    /* The mapped memory window and the I/O port window; the accessors read
     * and write through them. */
    uintptr_t mmio;
    uint16_t iobase;

    pci_device_t pci;
    int up;

    /* The card's own hardware address, from its EEPROM or its registers. */
    uint8_t mac[6];

    /* What the boot detail line reports. The receive counters separate the
     * reasons a frame is refused, because on this card they mean different
     * things: a hardware error, a frame the card split across descriptors,
     * and a length outside what a buffer can hold. */
    uint32_t rx_packets;
    uint32_t rx_errors;
    uint32_t rx_dropped;
    uint32_t tx_packets;
    uint32_t tx_stalls;

    /* The rings and their buffers, taken from the identity-mapped window when
     * the card comes up rather than compiled in: a machine with one card
     * takes one card's worth, and one with four takes four. The window is
     * where the two addresses of a page are the same, which is what a card
     * given an address for DMA needs. */
    volatile struct e1000_rx_desc *rx_ring;
    volatile struct e1000_tx_desc *tx_ring;
    uint8_t *rx_buf[E1000_RING_SIZE];
    uint8_t *tx_buf[E1000_RING_SIZE];

    uint32_t rx_cur; /* next receive descriptor to inspect */
    /* Submitted and reclaimed counts, not ring indices: they only ever
       increase, and their difference is what is outstanding. Indices come
       from them by remainder. Keeping the count is what lets the ring be full
       without being mistaken for empty; see e1000_tx_frame. */
    uint32_t tx_head;
    uint32_t tx_tail;

    /* Set while a frame the card split across descriptors is being discarded,
       so the descriptors after the first are dropped too rather than
       delivered as frames of their own. Cleared by the one carrying
       end-of-packet. */
    int rx_discarding;
};

static uint32_t mmio_r32(struct e1000 *e, uint32_t off)
{
    return *(volatile uint32_t *)(e->mmio + off);
}

static void mmio_w32(struct e1000 *e, uint32_t off, uint32_t value)
{
    *(volatile uint32_t *)(e->mmio + off) = value;
}

/**
 * Write one register through the card's I/O window.
 *
 * The window is an address register and a data register, so a write is two
 * port writes. Only the reset goes this way.
 */
static void mmio_wio(struct e1000 *e, uint32_t off, uint32_t value)
{
    outl(e->iobase + E1000_IO_ADDR, off);
    outl(e->iobase + E1000_IO_DATA, value);
}

static const struct e1000_io mmio_io = {mmio_r32, mmio_w32, mmio_wio, k_udelay};

/* The binding a card found from now on uses. The host test points this at
 * its model before the probe runs, and a card keeps what it was made with. */
static const struct e1000_io *io = &mmio_io;

/* ------------------------------------------------------------------ */
/* Driver state.                                                       */
/* ------------------------------------------------------------------ */

static struct e1000 e1000s[E1000_MAX];
static const char *e1000_err = "no e1000 present";

static void e1000_irq(isr_params_t *params);
static void e1000_service(struct e1000 *e);
static void e1000_rx(struct e1000 *e);
static int e1000_tx_frame(struct e1000 *e, const uint8_t *data, uint16_t len);

/* ------------------------------------------------------------------ */
/* Bring-up.                                                           */
/* ------------------------------------------------------------------ */

/**
 * Quiesce the device, reset it, and wait for the handshake to complete.
 *
 * The receiver and transmitter are turned off first and given a moment to
 * settle. This kernel has not started them, but a global reset preserves PCI
 * configuration, so a card left running and bus-mastering by firmware or a
 * network boot arrives here still fetching descriptors against addresses that
 * mean nothing to this kernel. Resetting such a card with transfers in flight
 * is how memory gets corrupted by a device nobody has configured.
 *
 * Interrupts are masked before the reset and again after it, because a reset
 * restores the mask to its power-on value and an unmasked cause with no
 * handler registered would be an interrupt nobody answers.
 *
 * The reset itself is written through the I/O window, not the memory window,
 * for the erratum described where `E1000_IO_ADDR` is defined.
 *
 * `CTRL.RST` is self-clearing: the card raises it and drops it again when the
 * reset is done.
 *
 * @return 1 when the card came out of reset, 0 when the budget ran out.
 */
static int e1000_reset(struct e1000 *e)
{
    uint32_t i;

    io->w32(e, E1000_RCTL, 0);
    io->w32(e, E1000_TCTL, 0);
    io->w32(e, E1000_IMC, 0xffffffffu);
    (void)io->r32(e, E1000_ICR);
    /* Long enough for a transfer already begun to finish, which is what the
     * reference driver waits for at this point. */
    io->udelay(E1000_QUIESCE_USEC);

    io->wio(e, E1000_CTRL, io->r32(e, E1000_CTRL) | E1000_CTRL_RST);

    for (i = 0; i < E1000_RESET_STEPS; i++) {
        io->udelay(E1000_RESET_USEC);
        if ((io->r32(e, E1000_CTRL) & E1000_CTRL_RST) == 0) {
            io->w32(e, E1000_IMC, 0xffffffffu);
            (void)io->r32(e, E1000_ICR);
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
 * 82540EM's; the 82541 and 82547 moved both the address field and the done
 * bit, so a card that never reports done is one this driver should not be
 * reading, and the caller treats a 0 return as that rather than retrying.
 *
 * @return 1 on success with `*out` set, 0 when the card did not answer.
 */
static int e1000_eeprom_read(struct e1000 *e, uint16_t word, uint16_t *out)
{
    uint32_t value;
    uint32_t i;

    io->w32(e, E1000_EERD, E1000_EERD_START | ((uint32_t)word << E1000_EERD_ADDR_SHIFT));

    for (i = 0; i < E1000_EEPROM_STEPS; i++) {
        value = io->r32(e, E1000_EERD);
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
static int e1000_read_mac(struct e1000 *e, uint8_t *mac)
{
    uint32_t low;
    uint32_t high;
    uint16_t word;
    int i;

    high = io->r32(e, E1000_RAH0);
    if (high & E1000_RAH_AV) {
        low = io->r32(e, E1000_RAL0);
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
        if (!e1000_eeprom_read(e, (uint16_t)i, &word)) {
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
static void e1000_set_mac(struct e1000 *e, const uint8_t *mac)
{
    uint32_t low = (uint32_t)mac[0] | ((uint32_t)mac[1] << 8) | ((uint32_t)mac[2] << 16) |
                   ((uint32_t)mac[3] << 24);
    uint32_t high = (uint32_t)mac[4] | ((uint32_t)mac[5] << 8) | E1000_RAH_AV;

    io->w32(e, E1000_RAL0, low);
    io->w32(e, E1000_RAH0, high);
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
static void e1000_rx_setup(struct e1000 *e)
{
    uint32_t i;

    for (i = 0; i < E1000_RING_SIZE; i++) {
        e->rx_ring[i].addr_lo = (uint32_t)(uintptr_t)e->rx_buf[i];
        e->rx_ring[i].addr_hi = 0;
        e->rx_ring[i].len_csum = 0;
        e->rx_ring[i].sta_err = 0;
    }
    e->rx_cur = 0;
    e->rx_discarding = 0;

    io->w32(e, E1000_RDBAL, (uint32_t)(uintptr_t)e->rx_ring);
    io->w32(e, E1000_RDBAH, 0);
    io->w32(e, E1000_RDLEN, E1000_RING_BYTES);
    io->w32(e, E1000_RDH, 0);
    io->w32(e, E1000_RDT, E1000_RING_SIZE - 1);
}

/**
 * Program the transmit ring, empty.
 *
 * Nothing is marked done, because a descriptor's `DD` bit is read only for a
 * descriptor this driver has submitted: `e->tx_tail` never passes
 * `e->tx_head`, so an entry that has never been used is never examined.
 */
static void e1000_tx_setup(struct e1000 *e)
{
    uint32_t i;

    for (i = 0; i < E1000_RING_SIZE; i++) {
        e->tx_ring[i].addr_lo = 0;
        e->tx_ring[i].addr_hi = 0;
        e->tx_ring[i].lower = 0;
        e->tx_ring[i].upper = 0;
    }
    e->tx_head = 0;
    e->tx_tail = 0;

    io->w32(e, E1000_TDBAL, (uint32_t)(uintptr_t)e->tx_ring);
    io->w32(e, E1000_TDBAH, 0);
    io->w32(e, E1000_TDLEN, E1000_RING_BYTES);
    io->w32(e, E1000_TDH, 0);
    io->w32(e, E1000_TDT, 0);
    io->w32(e, E1000_TIPG,
            E1000_TIPG_IPGT | (E1000_TIPG_IPGR1 << E1000_TIPG_IPGR1_SHIFT) |
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
/*
 * Take the pages this card's rings and buffers live in.
 *
 * The two rings are 512 bytes each and share one page. A buffer is 2048
 * bytes, so two of them share a page and sixteen pages hold the ring's
 * thirty-two. Taken once: a card brought up a second time keeps what it has.
 *
 * @return 0, or -1 when the window has no more frames.
 */
static int e1000_pages(struct e1000 *e)
{
    uint32_t i;

    if (!e->rx_ring) {
        void *page = alloc_kernel_frame();

        if (!page) {
            return -1;
        }
        e->rx_ring = (volatile struct e1000_rx_desc *)page;
        /* The transmit ring is in the page's second half, counted in
         * receive descriptors so the arithmetic is pointer arithmetic on
         * any host. */
        e->tx_ring =
            (volatile struct e1000_tx_desc *)(e->rx_ring + 2048 / sizeof(struct e1000_rx_desc));
    }
    for (i = 0; i < E1000_RING_SIZE; i += 2) {
        if (!e->rx_buf[i]) {
            uint8_t *page = (uint8_t *)alloc_kernel_frame();

            if (!page) {
                return -1;
            }
            e->rx_buf[i] = page;
            e->rx_buf[i + 1] = page + E1000_RX_BUF_SIZE;
        }
        if (!e->tx_buf[i]) {
            uint8_t *page = (uint8_t *)alloc_kernel_frame();

            if (!page) {
                return -1;
            }
            e->tx_buf[i] = page;
            e->tx_buf[i + 1] = page + E1000_TX_BUF_SIZE;
        }
    }
    return 0;
}

static int e1000_init(struct netdev *nd, int nth)
{
    uint32_t addr;
    uintptr_t mapped;
    uint32_t bar = 0;
    uint32_t ctrl;
    uint32_t i;
    struct e1000 *e;

    if (nth < 0 || nth >= E1000_MAX) {
        return NETDEV_ABSENT; /* more of these cards than this kernel drives */
    }
    e = &e1000s[nth];
    {
        /* The pages this card already holds survive a second bring-up: they
         * are the window's, and a record cleared over them would lose them
         * for the life of the machine. */
        volatile struct e1000_rx_desc *rx = e->rx_ring;
        volatile struct e1000_tx_desc *tx = e->tx_ring;
        uint8_t *rb[E1000_RING_SIZE];
        uint8_t *tb[E1000_RING_SIZE];
        uint32_t b;

        for (b = 0; b < E1000_RING_SIZE; b++) {
            rb[b] = e->rx_buf[b];
            tb[b] = e->tx_buf[b];
        }
        k_memset(e, 0, sizeof(*e));
        e->rx_ring = rx;
        e->tx_ring = tx;
        for (b = 0; b < E1000_RING_SIZE; b++) {
            e->rx_buf[b] = rb[b];
            e->tx_buf[b] = tb[b];
        }
    }
    e->number = nth;
    e->nd = nd;
    nd->dev = e;

    e->up = 0;
    if (!pci_find_nth_by_id(E1000_VENDOR, E1000_DEVICE, nth, &e->pci)) {
        e1000_err = "no e1000 present";
        return NETDEV_ABSENT;
    }

    /* This kernel is PIC-only, so the sixteen legacy lines are the whole of
     * what can be delivered. A larger value from firmware is not merely a
     * line no interrupt arrives on: the vector is `32 + irq` in a byte, so a
     * big enough one wraps onto an exception vector and installs this
     * driver's handler there. Reject anything outside the range rather than
     * register a vector the card will never raise; this driver has no polled
     * path to fall back to. */
    if (e->pci.irq > 15) {
        e1000_err = "the e1000 has no usable IRQ line";
        return NETDEV_FAILED;
    }

    /* Both windows, by index. The memory window carries every register; the
     * I/O window carries the reset alone, and the card is refused without it
     * rather than reset the way the erratum warns against. */
    e->iobase = 0;
    for (i = 0; i < 6; i++) {
        if (pci_bar_is_memory(e->pci.bar[i])) {
            if (bar == 0 && pci_bar_address(e->pci.bar[i]) != 0) {
                bar = e->pci.bar[i];
            }
            if (pci_bar_is_64bit(e->pci.bar[i])) {
                i++;
            }
        } else if (e->iobase == 0 && pci_bar_address(e->pci.bar[i]) != 0) {
            e->iobase = (uint16_t)pci_bar_address(e->pci.bar[i]);
        }
    }
    if (bar == 0) {
        e1000_err = "the e1000 has no memory window";
        return NETDEV_FAILED;
    }
    if (e->iobase == 0) {
        e1000_err = "the e1000 has no I/O window, which its reset needs";
        return NETDEV_FAILED;
    }
    addr = pci_bar_address(bar);
    mapped = map_mmio(addr, E1000_BAR_WINDOW);
    if (mapped == 0) {
        e1000_err = "the e1000 window would not map";
        return NETDEV_FAILED;
    }
    e->mmio = mapped;
    pci_enable_memory_space(&e->pci);

    /* A window that answers every read with all ones is a card that is not
     * decoding it, and every check after this one would read the same. */
    if (io->r32(e, E1000_STATUS) == 0xffffffffu) {
        e1000_err = "the e1000 window reads back all ones";
        return NETDEV_FAILED;
    }

    if (!e1000_reset(e)) {
        e1000_err = "the e1000 did not come out of reset";
        return NETDEV_FAILED;
    }

    if (!e1000_read_mac(e, e->mac)) {
        e1000_err = "the e1000 reports no usable station address";
        return NETDEV_FAILED;
    }

    /* Ask for the link and let the card negotiate its own speed and duplex.
     * The two force bits are cleared with it: auto-speed detection is only
     * permitted while they are clear, their power-on values come from the
     * EEPROM rather than being known, and a MAC forced to a gigabit while its
     * PHY negotiated a hundred megabits is a link that carries nothing. VLAN
     * tagging is cleared because nothing above this layer reads a tag, and a
     * card left in VLAN mode would strip four bytes the stack expects to
     * see. */
    ctrl = io->r32(e, E1000_CTRL);
    ctrl |= E1000_CTRL_SLU | E1000_CTRL_ASDE;
    ctrl &= ~(E1000_CTRL_VME | E1000_CTRL_PHY_RST | E1000_CTRL_FRCSPD | E1000_CTRL_FRCDPLX);
    io->w32(e, E1000_CTRL, ctrl);

    /* An unwritten multicast table is not defined after a reset, and a stale
     * entry would admit traffic this machine did not ask for. */
    for (i = 0; i < E1000_MTA_ENTRIES; i++) {
        io->w32(e, E1000_MTA + i * 4, 0);
    }
    e1000_set_mac(e, e->mac);
    {
        int b;

        for (b = 0; b < 6; b++) {
            e->nd->mac[b] = e->mac[b];
        }
    }

    if (e1000_pages(e) != 0) {
        e1000_err = "the card's rings and buffers could not be taken";
        return NETDEV_FAILED;
    }
    e1000_rx_setup(e);
    e1000_tx_setup(e);

    /* The rings are complete before the card is given the bus. Whatever state
     * it arrived in, the reset above turned its receiver and transmitter off
     * and left them off, so from here it fetches only what this driver has
     * described. */
    pci_enable_bus_master(&e->pci);

    register_isr_handler((uint8_t)(32 + e->pci.irq), e1000_irq, "e1000");

    io->w32(e, E1000_RCTL, E1000_RCTL_EN | E1000_RCTL_BAM | E1000_RCTL_SZ_2048 | E1000_RCTL_SECRC);
    io->w32(e, E1000_TCTL,
            E1000_TCTL_EN | E1000_TCTL_PSP | E1000_TCTL_RTLC |
                ((uint32_t)E1000_TCTL_CT_DEFAULT << E1000_TCTL_CT_SHIFT) |
                ((uint32_t)E1000_TCTL_COLD_FD << E1000_TCTL_COLD_SHIFT));

    /* The card is serviceable before its sources are unmasked, and not the
     * other way round. The handler passes over a card whose `up` is clear, so
     * an interrupt raised between the unmask and this assignment would be
     * taken, do nothing, and leave the line asserted; on a level line that is
     * a machine that never finishes its own bring-up. The bring-up runs with
     * interrupts enabled, so that window is real. */
    e->up = 1;
    io->w32(e, E1000_IMS, E1000_INT_MASK);
    return NETDEV_UP;
}

/* ------------------------------------------------------------------ */
/* Transmit.                                                           */
/* ------------------------------------------------------------------ */

/**
 * Reclaim every transmit descriptor the card has finished with.
 *
 * `RS` is set on each descriptor submitted, so the card writes `DD` back when
 * it is done with one. Only descriptors between the reclaimed count and the
 * submitted count are examined, and they are examined in submission order,
 * which the card completes them in.
 */
static void e1000_tx_reclaim(struct e1000 *e)
{
    while (e->tx_tail != e->tx_head) {
        uint32_t idx = e->tx_tail % E1000_RING_SIZE;

        if (!(E1000_TXD_STATUS(&e->tx_ring[idx]) & E1000_TXD_STAT_DD)) {
            break;
        }
        /* The descriptor's other fields are read only after the card has
         * given it back. */
        __asm__ __volatile__("" : : : "memory");
        e->tx_tail++;
    }
}

/**
 * Copy a frame into the next transmit descriptor and hand it to the card.
 *
 * At most `E1000_RING_SIZE - 1` descriptors are ever outstanding, and the one
 * left over is not a rounding error. The card reads head equal to tail as an
 * empty ring: filling all 32 entries would wrap the tail onto a head the card
 * has not moved, and 32 descriptors it had not finished would become
 * invisible to it. Nothing would then ever be sent again. The count of
 * submissions minus the count of reclaims is what this bounds, which is why
 * those two are counts and not indices.
 *
 * A full ring waits, briefly. Dropping the moment a burst outruns the card
 * would lose frames that a millisecond would have made room for, so this
 * reclaims for a bounded interval and counts a stall only if the ring is
 * still full after that; a wedged card therefore costs one bounded wait per
 * frame rather than a kernel that stops.
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
static int e1000_tx_frame(struct e1000 *e, const uint8_t *data, uint16_t len)
{
    uint32_t idx;
    uint32_t i;
    uint16_t total = len;
    uint32_t flags;

    if (!e->up || len == 0 || len > E1000_FRAME_MAX) {
        if (len > E1000_FRAME_MAX) {
            klog(KLOG_WARN, "e1000", "tx: frame of %d bytes is too large, dropped", (int)len);
        }
        return NETDEV_TX_DROPPED;
    }

    flags = net_lock();
    e1000_tx_reclaim(e);

    if (e->tx_head - e->tx_tail >= E1000_RING_SIZE - 1) {
        for (i = 0; i < E1000_TX_WAIT_STEPS; i++) {
            io->udelay(E1000_TX_WAIT_USEC);
            e1000_tx_reclaim(e);
            if (e->tx_head - e->tx_tail < E1000_RING_SIZE - 1) {
                break;
            }
        }
        if (e->tx_head - e->tx_tail >= E1000_RING_SIZE - 1) {
            e->tx_stalls++;
            net_unlock(flags);
            return NETDEV_TX_DROPPED;
        }
    }
    idx = e->tx_head % E1000_RING_SIZE;

    /* One call, which moves words: a byte at a time here cost about
     * 12000 cycles a frame, which was the whole of this path's time. */
    k_memcpy(e->tx_buf[idx], data, len);
    /* Pad to the Ethernet minimum with zeroes, so no stale buffer bytes reach
     * the wire. Written as a loop, as rtl8168.c writes its own: this file is
     * compiled natively by its host model as well as freestanding by the
     * kernel, and it reaches for nothing outside itself. */
    if (total < 60) {
        k_memset(e->tx_buf[idx] + total, 0, (size_t)(60 - total));
        total = 60;
    }

    e->tx_ring[idx].addr_lo = (uint32_t)(uintptr_t)e->tx_buf[idx];
    e->tx_ring[idx].addr_hi = 0;
    e->tx_ring[idx].upper = 0; /* clear DD; the card sets it when done */
    e->tx_ring[idx].lower =
        E1000_TXD_LOWER(total, E1000_TXD_CMD_EOP | E1000_TXD_CMD_IFCS | E1000_TXD_CMD_RS);

    /* Every field of the descriptor must be visible before the tail moves,
     * because the tail is what tells the card to read it. */
    __asm__ __volatile__("" : : : "memory");
    e->tx_head++;
    io->w32(e, E1000_TDT, e->tx_head % E1000_RING_SIZE);

    e->tx_packets++;
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
 * failing drops it and counts it. A descriptor without end-of-packet also
 * puts the receiver into discard until one arrives, because the rest of that
 * frame is not a frame; without it the tail of a split frame would be handed
 * up as though it were whole. The error byte must be clear, because a
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
static void e1000_rx(struct e1000 *e)
{
    uint32_t taken = 0;
    uint32_t last = 0;
    uint32_t budget;

    for (budget = 0; budget < E1000_RING_SIZE; budget++) {
        volatile struct e1000_rx_desc *d = &e->rx_ring[e->rx_cur];
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

        if (e->rx_discarding) {
            /* The tail of a frame whose head was already refused. It is not a
             * frame of its own, and delivering it would hand the stack the
             * second half of something as though it were whole. */
            e->rx_dropped++;
            ethernet_note_rx_drop(e->nd);
            if (status & E1000_RXD_STAT_EOP) {
                e->rx_discarding = 0;
            }
        } else if (errors & E1000_RXD_ERR_ANY) {
            e->rx_errors++;
            ethernet_note_rx_error(e->nd);
            e->rx_discarding = (status & E1000_RXD_STAT_EOP) ? 0 : 1;
        } else if (!(status & E1000_RXD_STAT_EOP)) {
            e->rx_dropped++;
            ethernet_note_rx_drop(e->nd);
            e->rx_discarding = 1;
        } else if (len < E1000_FRAME_MIN || len > E1000_RX_BUF_SIZE) {
            e->rx_dropped++;
            ethernet_note_rx_drop(e->nd);
        } else {
            e->rx_packets++;
            ethernet_rx(e->nd, e->rx_buf[e->rx_cur], (uint16_t)len);
        }

        d->len_csum = 0;
        d->sta_err = 0;
        __asm__ __volatile__("" : : : "memory");

        last = e->rx_cur;
        taken++;
        e->rx_cur = (e->rx_cur + 1) % E1000_RING_SIZE;
    }

    if (taken != 0) {
        io->w32(e, E1000_RDT, last);
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
    int n;

    (void)params;

    /* The line is shared, and by more than one of these cards on a machine
     * that has two. Every card this driver brought up is asked, and one with
     * nothing pending costs a register read. */
    for (n = 0; n < E1000_MAX; n++) {
        struct e1000 *e = &e1000s[n];

        if (e->up) {
            e1000_service(e);
        }
    }
}

/** Service one card until its cause register reads clear. */
static void e1000_service(struct e1000 *e)
{
    uint32_t cause;
    int guard;

    for (guard = 0; guard < 64; guard++) {
        cause = io->r32(e, E1000_ICR);
        if ((cause & E1000_INT_MASK) == 0) {
            break;
        }
        if (cause & (E1000_INT_RXT0 | E1000_INT_RXDMT0)) {
            e1000_rx(e);
        }
        if (cause & E1000_INT_RXO) {
            /* The ring filled while the machine was elsewhere. Draining it is
             * the whole recovery: the descriptors the card could not use are
             * returned by the tail write e1000_rx ends with. */
            e->rx_dropped++;
            ethernet_note_rx_drop(e->nd);
            e1000_rx(e);
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
 * Report the link, from the card's own status register.
 *
 * The speed field is two bits and both 2 and 3 mean a gigabit, which is why
 * this reads as two tests rather than a table: the card has three speeds and
 * four encodings for them.
 *
 * @return 1 with `*out` filled; 0 only when no window is mapped, since a card
 *         whose registers can be read can always answer this.
 */
static int e1000_link(struct e1000 *e, struct netdev_link *out)
{
    uint32_t status;
    uint32_t speed;

    if (e->mmio == 0) {
        return 0;
    }
    status = io->r32(e, E1000_STATUS);
    speed = (status & E1000_STATUS_SPEED_MASK) >> E1000_STATUS_SPEED_SHIFT;
    out->up = (status & E1000_STATUS_LU) ? 1 : 0;
    out->mbps = (uint16_t)(speed == 0 ? 10 : (speed == 1 ? 100 : 1000));
    out->full_duplex = (status & E1000_STATUS_FD) ? 1 : 0;
    return 1;
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
 * What the card already accepted is given a bounded moment to reach the wire
 * first. The shutdown path submits its last log line and then stops the
 * device, and a transmit returns once the descriptor is posted, not once the
 * frame has left; without this drain the message a remote reader is waiting
 * for would be discarded in the ring it was just placed in. A card that never
 * finishes costs the bounded wait and no more.
 *
 * Safe on a card that never finished starting. `e1000_init` enables bus
 * mastering before it enables the receiver, and can still fail after that, so
 * the probe calls this on a driver that reported `NETDEV_FAILED` and the
 * window may hold a half-configured card. The PCI record is written only when
 * a device was found, which `e->up` does not tell us; the window pointer
 * does, because nothing is written to a card whose window never mapped.
 */
static void e1000_stop(struct e1000 *e)
{
    uint32_t i;

    if (e->mmio != 0) {
        for (i = 0; i < E1000_DRAIN_STEPS; i++) {
            e1000_tx_reclaim(e);
            if (e->tx_tail == e->tx_head) {
                break;
            }
            io->udelay(E1000_DRAIN_USEC);
        }
        io->w32(e, E1000_RCTL, 0);
        io->w32(e, E1000_TCTL, 0);
        io->w32(e, E1000_IMC, 0xffffffffu);
        (void)io->r32(e, E1000_ICR);
    }
    if (e->pci.vendor_id == E1000_VENDOR && e->pci.device_id == E1000_DEVICE) {
        pci_disable_bus_master(&e->pci);
    }
    e->up = 0;
}

/**
 * The card's detail lines under the boot summary.
 *
 * The kernel formatter has no field width, so the station address is printed a
 * nibble at a time, as the other two drivers print theirs.
 */
static void e1000_print_info(struct e1000 *e)
{
    struct netdev_link link;

    /* The window and the vector are for whoever is debugging the card; the
       station address is on the `net: eth0:` line above, where a person looks
       for it. */
    klog(KLOG_DEBUG, "e1000", "82540EM, MMIO 0x%X, IRQ %d, MAC %X%X:%X%X:%X%X:%X%X:%X%X:%X%X",
         (unsigned)(uintptr_t)e->mmio, (int)e->pci.irq, (unsigned)(e->mac[0] >> 4),
         (unsigned)(e->mac[0] & 0xF), (unsigned)(e->mac[1] >> 4), (unsigned)(e->mac[1] & 0xF),
         (unsigned)(e->mac[2] >> 4), (unsigned)(e->mac[2] & 0xF), (unsigned)(e->mac[3] >> 4),
         (unsigned)(e->mac[3] & 0xF), (unsigned)(e->mac[4] >> 4), (unsigned)(e->mac[4] & 0xF),
         (unsigned)(e->mac[5] >> 4), (unsigned)(e->mac[5] & 0xF));
    if (e1000_link(e, &link) && link.up) {
        klog(KLOG_INFO, "e1000", "link %d Mb/s, %s duplex", (int)link.mbps,
             link.full_duplex ? "full" : "half");
    } else {
        klog(KLOG_INFO, "e1000", "no link yet");
    }
    /* Worth a line once they count something; at bring-up they are all zero
       and the line says nothing. */
    if (e->rx_packets || e->rx_errors || e->rx_dropped || e->tx_packets || e->tx_stalls) {
        klog(KLOG_INFO, "e1000", "rx %d (%d error, %d dropped), tx %d (%d stalled)", e->rx_packets,
             e->rx_errors, e->rx_dropped, e->tx_packets, e->tx_stalls);
    }
}

/* The card's entry in the driver table. `poll` is NULL: this one
 * interrupts. */
/*
 * What the interface layer calls. It holds a card as an opaque record, so
 * these take it back to this driver's own type; everything above works in
 * interfaces and does not know what a card is.
 */
static int e1000_op_tx(void *dev, const uint8_t *data, uint16_t len)
{
    return e1000_tx_frame((struct e1000 *)dev, data, len);
}

static void e1000_op_stop(void *dev)
{
    e1000_stop((struct e1000 *)dev);
}

static int e1000_op_link(void *dev, struct netdev_link *out)
{
    return e1000_link((struct e1000 *)dev, out);
}

static void e1000_op_print_info(void *dev)
{
    e1000_print_info((struct e1000 *)dev);
}

const struct netdev_ops e1000_netdev = {"e1000",     e1000_init,          e1000_op_tx,
                                        0,           e1000_op_stop,       e1000_op_link,
                                        e1000_error, e1000_op_print_info, 0};
