/*
 * e1000.c: the driver of up to E1000_MAX Intel 82540EM cards.
 *
 * The card is the descriptor-ring design with nothing else on top. Two rings
 * of 16-byte descriptors are in kernel memory; the card owns a head index
 * into each and this driver owns a tail. To give the card a receive buffer,
 * advance the receive tail; to send, fill a transmit descriptor and advance
 * the transmit tail. The card writes a done bit back into each descriptor it
 * finishes. That is the whole mechanism, and the RTL8168's rings are the
 * same design with more on top, which is why this driver is the one to read
 * before rtl8168.c.
 *
 * The card interrupts, so `poll` is NULL. It shares its PCI line, so the
 * handler services the card until its cause register reads clear, the same
 * rule rtl8139_irq follows and for the same reason.
 *
 * All register access goes through an indirection (`io`) so the host test can
 * drive the whole bring-up against a software model of the chip. The default
 * binding is plain MMIO on the mapped window, and port I/O for the reset
 * alone. The card is emulated by QEMU, so unlike the RTL8168GU this driver
 * can also be run; the model exists to reach the failures QEMU does not
 * produce.
 */

#include "../../klog.h"
#include "../../lib/kstring.h"
#include "e1000.h"
#include "../net.h"
#include "../ethernet/ethernet.h"
#include "../../irq.h"
#include "../../kio.h"
#include "../../mm/paging.h"
#include "../../pci.h"
#include "../../serial.h"
#include "../../udelay.h"
#include "../../mm/paging.h"

/* Register access, behind an indirection the host test can replace. */

struct e1000;

/**
 * The chip's registers and the short waits its handshakes need.
 *
 * Every register on this card is 32 bits wide, so unlike the RTL8168GU's
 * accessors there is one width here and no others are offered. `udelay`
 * waits at least as long as asked: each use below is a poll interval, and
 * waiting longer than asked is always safe. The kernel's binding is
 * `k_udelay`, which measures against a hardware counter, because a counted
 * loop cannot promise a duration on a processor whose speed the kernel never
 * learns.
 */
struct e1000_io {
    uint32_t (*r32)(struct e1000 *e, uint32_t off);
    void (*w32)(struct e1000 *e, uint32_t off, uint32_t value);
    /* A write through the card's I/O window instead of its memory window.
       Only the reset uses it; see the note on E1000_IO_ADDR. */
    void (*wio)(struct e1000 *e, uint32_t off, uint32_t value);
    void (*udelay)(uint32_t usec);
};

/* How many of these cards this kernel drives. A machine with two of them has
 * two records; each has its own registers, rings and counters, and nothing
 * one of them does reaches the other. */
#define E1000_MAX 4

/*
 * One card. The rings and buffers use frames of the DMA pool
 * (`alloc_kernel_frame`), below HARD_MEMORY_LIMIT and so below 4 GiB, and
 * the card is given their physical addresses, `virt_to_phys` of the
 * kernel's, with the high halves 0. The manual asks for 16-byte alignment
 * on each ring.
 *
 * The rings are volatile and the buffers are not, which is the distinction
 * between memory the card writes while this driver is reading it and memory
 * only one of them touches at a time. A done bit the compiler kept in a
 * register would be a wait that never ends. The kernel builds this file
 * without optimization, where that cannot happen, but the host model builds
 * the same file at -O1, so the type states the sharing whatever the flags.
 * A buffer is read only after its descriptor reported done, so making
 * 112 KiB of buffer volatile would slow every copy to no purpose.
 */
struct e1000 {
    int used;
    int number; /* its place in `e1000s`, the `nth` of its bring-up */
    /* The interface this card is, so a frame it delivers says where it
     * arrived and its counters are that interface's. */
    struct netdev *nd;

    /* The mapped memory window and the I/O port window; the accessors read
     * and write through them. */
    uintptr_t mmio;
    uint16_t iobase;

    pci_device_t pci;
    /* Set at the end of the bring-up, before the sources are unmasked;
     * transmit and the handler pass over a card without it. */
    int up;

    /* The card's own hardware address, from its EEPROM or its registers. */
    uint8_t mac[6];

    /* What the detail lines report. The receive counters separate a
     * hardware error (`rx_errors`) from a frame dropped because the card
     * split it across descriptors, because its length is outside what a
     * buffer can hold, or because the ring overflowed (`rx_dropped`).
     * `tx_stalls` counts the frames dropped for a ring that stayed full. */
    uint32_t rx_packets;
    uint32_t rx_errors;
    uint32_t rx_dropped;
    uint32_t tx_packets;
    uint32_t tx_stalls;

    /* The rings and their buffers, taken from the DMA pool when the card
     * first comes up and kept from then on: a machine with one card takes
     * one card's worth, and one with four takes four. */
    volatile struct e1000_rx_desc *rx_ring;
    volatile struct e1000_tx_desc *tx_ring;
    uint8_t *rx_buf[E1000_RING_SIZE];
    uint8_t *tx_buf[E1000_RING_SIZE];

    uint32_t rx_cur; /* next receive descriptor to inspect */
    /* The counts of descriptors submitted and reclaimed, which only
       increase: their difference is what is outstanding, and the ring
       indices are their remainders. A difference of counts tells a full
       ring from an empty one where two indices could not; see
       e1000_tx_frame. */
    uint32_t tx_head;
    uint32_t tx_tail;

    /* Set while a frame the card split across descriptors is being discarded,
       so the descriptors after the first are dropped too instead of
       delivered as frames of their own. Cleared by the one with
       end-of-packet set. */
    int rx_discarding;
};

/** Read the register at `off` of `e`'s memory window. */
static uint32_t mmio_r32(struct e1000 *e, uint32_t off)
{
    return mmio_read32(e->mmio + off);
}

/** Write the register at `off` of `e`'s memory window. */
static void mmio_w32(struct e1000 *e, uint32_t off, uint32_t value)
{
    mmio_write32(e->mmio + off, value);
}

/**
 * Write one register through the card's I/O window.
 *
 * The window is an address register and a data register, so a write is two
 * port writes. Only the reset goes this way.
 */
static void mmio_wio(struct e1000 *e, uint32_t off, uint32_t value)
{
    pci_io_write32((uint16_t)(e->iobase + E1000_IO_ADDR), off);
    pci_io_write32((uint16_t)(e->iobase + E1000_IO_DATA), value);
}

static const struct e1000_io mmio_io = {mmio_r32, mmio_w32, mmio_wio, k_udelay};

/* The binding every card uses. The host test points this at its model before
 * the probe runs. */
static const struct e1000_io *io = &mmio_io;

/* Driver state. */

static struct e1000 e1000s[E1000_MAX];
static const char *e1000_err = "no e1000 present"; /* the last init's reason */

static void e1000_irq(isr_params_t *params);
static void e1000_service(struct e1000 *e);
static void e1000_rx(struct e1000 *e);
static int e1000_tx_frame(struct e1000 *e, const uint8_t *data, uint16_t len);

/* Bring-up. */

/**
 * Stop the card's transfers, reset it, and wait for the reset to complete.
 *
 * The receiver and transmitter are turned off first and given
 * E1000_QUIESCE_USEC to settle. This kernel has not started them, but a
 * global reset preserves PCI configuration, so a card left running and
 * bus-mastering by firmware or a network boot arrives here still fetching
 * descriptors at addresses that mean nothing to this kernel, and resetting
 * it with transfers in flight can corrupt memory.
 *
 * Interrupts are masked, and the causes read clear, before the reset and
 * again after it, because a reset restores the mask to its power-on value
 * and an unmasked cause with no handler registered would be an interrupt
 * nothing answers.
 *
 * The reset itself is written through the I/O window, for the erratum
 * described where `E1000_IO_ADDR` is defined. `CTRL.RST` is self-clearing:
 * the card clears it when the reset is done.
 *
 * @return 1 when the card came out of reset, 0 when the waits ran out.
 */
static int e1000_reset(struct e1000 *e)
{
    uint32_t i;

    io->w32(e, E1000_RCTL, 0);
    io->w32(e, E1000_TCTL, 0);
    io->w32(e, E1000_IMC, 0xffffffffu);
    (void)io->r32(e, E1000_ICR);
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
 * reading, and the caller fails the bring-up on a 0 return instead of
 * trying again.
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
 * Whether `mac` is an address a card may transmit from: 1, or 0 for one it
 * may not.
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
 * Read the station address into `mac`.
 *
 * `RAL0` and `RAH0` are tried first: a card that loaded its address from its
 * own EEPROM at power-on has it there already, with the valid bit set, and
 * this is the path a QEMU machine takes. The EEPROM's first three words are
 * read when `RAH0.AV` is clear or the registers' address is not valid.
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
 * Program the receive ring and give every descriptor but one to the card.
 *
 * The ring is empty when head and tail are equal, so one descriptor is always
 * left out of the card's reach; without that gap a full ring could not be
 * told from an empty one. The base, length, and both indices are set before
 * `RCTL.EN`, which the caller sets afterwards: a receiver started on an
 * unprogrammed ring would fetch descriptors from address zero.
 */
static void e1000_rx_setup(struct e1000 *e)
{
    uint32_t i;

    for (i = 0; i < E1000_RING_SIZE; i++) {
        e->rx_ring[i].addr_lo = (uint32_t)virt_to_phys(e->rx_buf[i]);
        e->rx_ring[i].addr_hi = 0;
        e->rx_ring[i].len_csum = 0;
        e->rx_ring[i].sta_err = 0;
    }
    e->rx_cur = 0;
    e->rx_discarding = 0;

    io->w32(e, E1000_RDBAL, (uint32_t)virt_to_phys(e->rx_ring));
    io->w32(e, E1000_RDBAH, 0);
    io->w32(e, E1000_RDLEN, E1000_RING_BYTES);
    io->w32(e, E1000_RDH, 0);
    io->w32(e, E1000_RDT, E1000_RING_SIZE - 1);
}

/**
 * Program the transmit ring, empty, and the inter-packet gap.
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

    io->w32(e, E1000_TDBAL, (uint32_t)virt_to_phys(e->tx_ring));
    io->w32(e, E1000_TDBAH, 0);
    io->w32(e, E1000_TDLEN, E1000_RING_BYTES);
    io->w32(e, E1000_TDH, 0);
    io->w32(e, E1000_TDT, 0);
    io->w32(e, E1000_TIPG,
            E1000_TIPG_IPGT | (E1000_TIPG_IPGR1 << E1000_TIPG_IPGR1_SHIFT) |
                (E1000_TIPG_IPGR2 << E1000_TIPG_IPGR2_SHIFT));
}

/**
 * Take the pages of this card's rings and buffers that it does not have.
 *
 * The two rings share one page, the receive ring in its first half and the
 * transmit ring in its second. Each receive page has two 2048-byte buffers,
 * and each transmit page two 1536-byte buffers. The pages are kept from one
 * bring-up to the next and never given back.
 *
 * @return 0, or -1 when the pool has no more frames.
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

/**
 * Probe the card, reset it, program both rings, and start it. Card `nth`
 * is `e1000s[nth]`, and an `nth` of E1000_MAX or more is absent.
 *
 * The order is the one the manual asks for and the one the host model
 * checks: the window is mapped and answering before anything is written to
 * it, interrupts are masked before the reset, the station address and the
 * multicast table are set before the receiver could match a frame, both rings
 * are complete before bus mastering is enabled, the receiver and transmitter
 * are switched on after that, and the interrupt sources are unmasked last.
 * Nothing before the PCI lookup succeeds touches a register, which is what
 * the driver table requires of a card that is not in the machine.
 *
 * A second bring-up of the same card keeps the pages the first one took:
 * this driver never gives them back, so a record cleared over them would
 * lose them for the life of the machine. The platform's lines are 0 to 15,
 * at vectors `32 + irq`, whether the 8259 pair or the I/O APIC delivers
 * them; a line above 15 has no such vector, and 255, a function's line when
 * it has none, would wrap the byte onto an exception vector. So a line above
 * 15 is refused, and this driver has no polled path to fall back to.
 *
 * The memory window is the first memory BAR with an address, a 64-bit one
 * taking the BAR after it as its upper half, and the I/O window the first
 * I/O BAR with an address. The memory window is used for every register and
 * the I/O window for the reset alone, and a card without the I/O window is
 * refused instead of reset the way the erratum warns against. A window that
 * reads all ones is one the card does not decode, and every check after it
 * would read the same.
 *
 * The link is asked for and the card negotiates its own speed and duplex.
 * The two force bits are cleared: auto-speed detection is only permitted
 * while they are clear, their power-on values come from the EEPROM, and a
 * MAC forced to a gigabit while its PHY negotiated a hundred megabits passes
 * no frames. VLAN mode is cleared, since nothing above this layer reads a tag
 * and the card would strip four bytes the stack expects to see, and so is
 * the PHY reset bit. The multicast table is undefined after a reset, and is
 * cleared so that a stale entry admits no traffic this machine did not ask
 * for.
 *
 * `up` is set before the sources are unmasked. The handler passes over a
 * card whose `up` is clear, and the bring-up runs with interrupts enabled, so
 * a cause raised in between would be taken, left unserviced, and keep a level
 * line asserted.
 */
static int e1000_init(struct netdev *nd, int nth)
{
    uint64_t addr = 0;
    uintptr_t mapped;
    uint32_t ctrl;
    uint32_t i;
    struct e1000 *e;

    if (nth < 0 || nth >= E1000_MAX) {
        return NETDEV_ABSENT;
    }
    e = &e1000s[nth];
    {
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
    nd->pci_key = e->pci.bus << 8 | e->pci.device << 3 | e->pci.function;

    if (e->pci.irq > 15) {
        e1000_err = "the e1000 has no usable IRQ line";
        return NETDEV_FAILED;
    }

    e->iobase = 0;
    for (i = 0; i < 6; i++) {
        if (pci_bar_is_memory(e->pci.bar[i])) {
            if (addr == 0) {
                addr = pci_bar_address64(&e->pci, (int)i);
            }
            if (pci_bar_is_64bit(e->pci.bar[i])) {
                i++;
            }
        } else if (e->iobase == 0 && pci_bar_address(e->pci.bar[i]) != 0) {
            e->iobase = (uint16_t)pci_bar_address(e->pci.bar[i]);
        }
    }
    if (addr == 0) {
        e1000_err = "the e1000 has no memory window";
        return NETDEV_FAILED;
    }
    if ((uint64_t)(uintptr_t)addr != addr) {
        e1000_err = "the e1000 window is above the addresses this kernel maps";
        return NETDEV_FAILED;
    }
    if (e->iobase == 0) {
        e1000_err = "the e1000 has no I/O window, which its reset needs";
        return NETDEV_FAILED;
    }
    mapped = map_mmio((uintptr_t)addr, E1000_BAR_WINDOW);
    if (mapped == 0) {
        e1000_err = "the e1000 window would not map";
        return NETDEV_FAILED;
    }
    e->mmio = mapped;
    pci_enable_memory_space(&e->pci);

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

    ctrl = io->r32(e, E1000_CTRL);
    ctrl |= E1000_CTRL_SLU | E1000_CTRL_ASDE;
    ctrl &= ~(E1000_CTRL_VME | E1000_CTRL_PHY_RST | E1000_CTRL_FRCSPD | E1000_CTRL_FRCDPLX);
    io->w32(e, E1000_CTRL, ctrl);

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

    pci_enable_bus_master(&e->pci);

    register_isr_handler((uint8_t)IRQ_VECTOR(e->pci.irq), e1000_irq, "e1000");

    io->w32(e, E1000_RCTL, E1000_RCTL_EN | E1000_RCTL_BAM | E1000_RCTL_SZ_2048 | E1000_RCTL_SECRC);
    io->w32(e, E1000_TCTL,
            E1000_TCTL_EN | E1000_TCTL_PSP | E1000_TCTL_RTLC |
                ((uint32_t)E1000_TCTL_CT_DEFAULT << E1000_TCTL_CT_SHIFT) |
                ((uint32_t)E1000_TCTL_COLD_FD << E1000_TCTL_COLD_SHIFT));

    e->up = 1;
    io->w32(e, E1000_IMS, E1000_INT_MASK);
    return NETDEV_UP;
}

/* Transmit. */

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
        k_barrier(); /* the rest of the descriptor is read only after DD */
        e->tx_tail++;
    }
}

/**
 * Copy a frame into the next transmit descriptor and hand it to the card.
 *
 * At most `E1000_RING_SIZE - 1` descriptors are ever outstanding. The card
 * reads head equal to tail as an empty ring: filling all 32 entries would
 * wrap the tail onto a head the card has not moved, and 32 descriptors it had
 * not finished would become invisible to it. Nothing would then ever be sent
 * again. The count of submissions minus the count of reclaims is what this
 * bounds, which is why those two are counts and not indices.
 *
 * A full ring waits, briefly. Dropping the moment a burst outruns the card
 * would lose frames that a millisecond would have made room for, so this
 * reclaims for E1000_TX_WAIT_STEPS waits and counts a stall, and drops the
 * frame, only if the ring is still full after them; a stuck card therefore
 * costs one bounded wait per frame instead of a kernel that stops. A frame
 * longer than E1000_FRAME_MAX is dropped with a warning, and so is any frame
 * while the card is not up.
 *
 * Short frames are padded to the 60-byte Ethernet minimum with zeroes, so no
 * stale buffer bytes reach the wire. `TCTL.PSP` asks the card to do the same,
 * and both are set because an emulated card may implement one and not the
 * other, and the result would be a runt on the wire.
 *
 * The ring work runs under `net_lock`, as `rtl8168_tx`'s does: a sender in
 * an interrupt handler, a TCP reply to a received segment among them,
 * advances the same ring indices. The bracket nests, so a caller that
 * already holds it pays nothing.
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

    k_memcpy(e->tx_buf[idx], data, len);
    if (total < 60) {
        k_memset(e->tx_buf[idx] + total, 0, (size_t)(60 - total));
        total = 60;
    }

    e->tx_ring[idx].addr_lo = (uint32_t)virt_to_phys(e->tx_buf[idx]);
    e->tx_ring[idx].addr_hi = 0;
    e->tx_ring[idx].upper = 0; /* clear DD; the card sets it when done */
    e->tx_ring[idx].lower =
        E1000_TXD_LOWER(total, E1000_TXD_CMD_EOP | E1000_TXD_CMD_IFCS | E1000_TXD_CMD_RS);

    k_barrier(); /* the descriptor is whole before the tail moves */
    e->tx_head++;
    io->w32(e, E1000_TDT, e->tx_head % E1000_RING_SIZE);

    e->tx_packets++;
    net_unlock(flags);
    return NETDEV_TX_OK;
}

/* Receive. */

/**
 * Take finished frames off the receive ring and give them to `ethernet_rx`,
 * at most a ring's worth in one call.
 *
 * Three things are checked before a frame is delivered, and any one of them
 * failing drops it and counts it. The error byte must be clear, because a
 * descriptor can report done and end-of-packet and still report a receive
 * error. `EOP` must be set on the same descriptor, because a frame split
 * across two would need a reassembly this driver does not do, and at a
 * 1500-byte transfer unit against a 2048-byte buffer the card has no reason
 * to split one. The length must cover an Ethernet header, and it must not
 * exceed the buffer, because a length taken on trust would give the stack
 * bytes past the end of the array. A descriptor without end-of-packet, an
 * error's or a split frame's, also puts the receiver into discard until the
 * descriptor that ends the frame, which is dropped and counted too; without
 * that the rest of a split frame would be delivered as though it were
 * whole.
 *
 * `SECRC` is set, so the length is the frame's own and the frame check
 * sequence has already been removed. Each descriptor's status is cleared
 * before it goes back to the card, and the tail is written once at the end
 * instead of for each descriptor: one register write returns the whole
 * batch. Runs in the interrupt handler.
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
        k_barrier(); /* the descriptor's payload is read only after DD */

        errors = E1000_RXD_ERRORS(d);
        len = E1000_RXD_LEN(d);

        if (e->rx_discarding) {
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
        k_barrier();

        last = e->rx_cur;
        taken++;
        e->rx_cur = (e->rx_cur + 1) % E1000_RING_SIZE;
    }

    if (taken != 0) {
        io->w32(e, E1000_RDT, last);
    }
}

/**
 * The cards' interrupt handler: service every card this driver brought up.
 *
 * A PCI line is shared, with other devices and with a second card of this
 * kind, so every card that is up is asked, and one with nothing pending costs
 * a register read. The interrupt router ends the interrupt at its
 * controller, so this handler sends no end of interrupt.
 */
static void e1000_irq(isr_params_t *params)
{
    int n;

    (void)params;

    for (n = 0; n < E1000_MAX; n++) {
        struct e1000 *e = &e1000s[n];

        if (e->up) {
            e1000_service(e);
        }
    }
}

/**
 * Service one card until its cause register reads clear.
 *
 * A frame arriving after the cause was read but before the ring drained would
 * raise it again. Reading in a loop until nothing is pending leaves the line
 * deasserted and the ring drained on return, so no arrival waits for the
 * next interrupt. The guard of 64 reads bounds the loop should a cause stay
 * set. Reading `ICR` is what clears it. An overflow is counted as a drop and
 * recovered by draining the ring: the descriptors are returned to the card
 * by the tail write e1000_rx ends with. A link change needs nothing done,
 * since the card renegotiates on its own and print_info reads the current
 * state.
 */
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
            e->rx_dropped++;
            ethernet_note_rx_drop(e->nd);
            e1000_rx(e);
        }
    }
}

/* What the layers above ask for. */

/** The reason the last `e1000_init` gave, for any card. */
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
 * Stop the card: receiver and transmitter off, interrupts masked, DMA
 * ended.
 *
 * The order matters. The enables go first, so the card stops fetching
 * descriptors; the mask follows, with the causes read clear, so the card
 * raises its line no more; bus mastering goes last, because it is the one
 * that ends DMA whatever state the card is in, and clearing it before the
 * card was told to stop would leave the card with transfers in flight.
 *
 * The frames the card has taken are first given E1000_DRAIN_STEPS waits to
 * be sent. The shutdown sends its last log line and then stops the card, and
 * a transmit returns once the descriptor is posted, before the frame has
 * left; without this wait the line a reader of netconsole is waiting for
 * would be discarded in the ring. A card that never finishes costs the
 * bounded wait and no more.
 *
 * Safe on a card that never finished starting: the probe calls this on a
 * driver that reported `NETDEV_FAILED`, whose window may be mapped with the
 * card half configured. The register writes depend on the window pointer
 * and not on `e->up`, which is set only at the end of the bring-up, so
 * nothing is written to a card whose window never mapped; bus mastering is
 * cleared whenever the PCI record shows a card was found.
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
 * The window, the interrupt line and the address are a debug record, since
 * the station address is on the interface's `net: ethN:` line above, where a
 * person looks for it. The counters are logged once they count something; at
 * bring-up they are all zero. The kernel formatter has no field width, so the
 * station address is printed a nibble at a time, as the other two drivers
 * print theirs.
 */
static void e1000_print_info(struct e1000 *e)
{
    struct netdev_link link;

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
    if (e->rx_packets || e->rx_errors || e->rx_dropped || e->tx_packets || e->tx_stalls) {
        klog(KLOG_INFO, "e1000", "rx %d (%d error, %d dropped), tx %d (%d stalled)", e->rx_packets,
             e->rx_errors, e->rx_dropped, e->tx_packets, e->tx_stalls);
    }
}

/*
 * The operations of the driver table. The interface layer keeps a card as
 * an opaque record, `nd->dev`, so these convert it back to this driver's own
 * type; everything above works in interfaces and does not know what a card
 * is.
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

/* The card's entry in the driver table. `poll` is NULL: this one
 * interrupts. `counters` is NULL too: the driver reads none of the card's
 * statistics registers. */
const struct netdev_ops e1000_netdev = {"e1000",     e1000_init,          e1000_op_tx,
                                        0,           e1000_op_stop,       e1000_op_link,
                                        e1000_error, e1000_op_print_info, 0};
