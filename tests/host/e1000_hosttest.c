/**
 * Host test for the e1000 driver (sys/network/driver/e1000.c).
 *
 * host-deps:
 *
 * Unlike the RTL8168GU, this card is emulated, so the driver can be run
 * before it is believed and a boot proves the ordinary path. What a boot
 * cannot reach is everything the card does when something is wrong: QEMU
 * never reports a receive error, never splits a frame across descriptors,
 * never reports a length outside the buffer, never stops answering a reset,
 * and never leaves a transmit descriptor unfinished. Those are the failures a
 * driver is written for and the ones nothing else would catch.
 *
 * So a software model of the chip answers every register the driver touches,
 * moves descriptors the way the manual says the card does, and records every
 * write in order. The test then drives the driver against it, breaks one
 * thing at a time, and asserts what the driver did.
 *
 * What it proves: the bring-up sequence and its order, the EEPROM read
 * protocol and the register-pair preference, the refusal of an unusable
 * station address, each of the four receive checks failing on its own, ring
 * wrap in both directions, the transmit descriptor contract, the bounded
 * stall, and that a card that is not in the machine is not touched at all.
 *
 * Returns the failed-check count (0 == pass).
 */

/* The kernel headers this pulls in define uintptr_t themselves, so the test
 * uses no host header and reports through its exit code, as the other
 * kernel-header tests do. */

/* ------------------------------------------------------------------ */
/* The stubs the driver links against.                                 */
/* ------------------------------------------------------------------ */

/* Recorded delivery from the driver's receive path. */
static unsigned char rx_last[2048];
static unsigned rx_last_len;
static unsigned rx_calls;

/* The interface a frame arrived on, which this model does not look
 * inside. */
struct netdev;

void ethernet_rx(struct netdev *nd, const unsigned char *data, unsigned short len)
{
    (void)nd;
    unsigned i;

    rx_calls++;
    rx_last_len = len;
    for (i = 0; i < len && i < sizeof(rx_last); i++) {
        rx_last[i] = data[i];
    }
}

int ethernet_tx(struct netdev *out, const unsigned char *dst, unsigned short type,
                const unsigned char *payload, unsigned short plen)
{
    (void)out;
    (void)dst;
    (void)type;
    (void)payload;
    (void)plen;
    return 0;
}

/* The refusal counters the drivers report to. The interface owns them; here
   they are counted so a check can say which one the driver reached. */
static unsigned note_rx_errors;
static unsigned note_rx_drops;

void ethernet_note_rx_error(struct netdev *nd)
{
    (void)nd;
    note_rx_errors++;
}

void ethernet_note_rx_drop(struct netdev *nd)
{
    (void)nd;
    note_rx_drops++;
}

void k_puts(const char *s)
{
    (void)s;
}

void k_printf(const char *fmt, ...)
{
    (void)fmt;
}

/* The driver's real delay reads the 8254; the model supplies its own, so this
 * exists only to satisfy the link. */
void k_udelay(unsigned int usec)
{
    (void)usec;
}

/* The kernel's interrupt bracket. There are no interrupts here, so the flag is
   a token; what the test holds the driver to is that every lock it takes it
   releases, which the balance below checks. */
static int lock_depth;
static int lock_ever_negative;

unsigned int net_lock(void)
{
    lock_depth++;
    return 0;
}

void net_unlock(unsigned int flags)
{
    (void)flags;
    lock_depth--;
    if (lock_depth < 0) {
        lock_ever_negative = 1;
    }
}

/* The PCI surface the probe uses. The model presents one e1000 whose memory
 * window is the second BAR, so the probe's walk has to step over an I/O BAR
 * to find it. */
static int pci_present = 1;
static unsigned char pci_irq_line = 11;

#include "isr.h"
#include "pci.h"
#include "network/driver/e1000.h"

/* The driver reports what it found through the kernel log, which this test
   has no use for; the records are dropped. */
void klog(int level, const char *subsystem, const char *format, ...)
{
    (void)level;
    (void)subsystem;
    (void)format;
}

/* The port instructions the kernel headers declare. Nothing here uses a port,
   so these exist only to satisfy the link. */
uint8_t inb(uint16_t port)
{
    (void)port;
    return 0;
}

void outb(uint16_t port, uint8_t v)
{
    (void)port;
    (void)v;
}

/* The driver's reset goes through the card's I/O window, which is what these
   are. The model supplies its own `wio`, so nothing here is reached; they
   exist to satisfy the link. */
void outd(uint16_t port, uint32_t v)
{
    (void)port;
    (void)v;
}

uint32_t ind(uint16_t port)
{
    (void)port;
    return 0;
}

int pci_find_device_by_id(unsigned short vendor, unsigned short device, pci_device_t *out)
{
    int i;

    if (!pci_present || vendor != E1000_VENDOR || device != E1000_DEVICE) {
        return 0;
    }
    out->vendor_id = vendor;
    out->device_id = device;
    out->bus = 0;
    out->device = 3;
    out->function = 0;
    out->irq = pci_irq_line;
    for (i = 0; i < 6; i++) {
        out->bar[i] = 0;
    }
    out->bar[0] = 0x0000c001u; /* an I/O BAR, which must be stepped over */
    out->bar[1] = 0xfebc0000u; /* the memory window */
    return 1;
}

int pci_bar_is_memory(unsigned int bar)
{
    return bar != 0 && (bar & 1u) == 0;
}

int pci_bar_is_64bit(unsigned int bar)
{
    return (bar & 0x6u) == 0x4u;
}

unsigned int pci_bar_address(unsigned int bar)
{
    return pci_bar_is_memory(bar) ? (bar & ~0xfu) : (bar & ~0x3u);
}

static int pci_master_enabled;
static int pci_memory_enabled;

static void model_record(unsigned off, unsigned value);

void pci_enable_bus_master(pci_device_t *dev)
{
    (void)dev;
    pci_master_enabled = 1;
}

void pci_enable_memory_space(pci_device_t *dev)
{
    (void)dev;
    pci_memory_enabled = 1;
}

/* Recorded in the write log as well as in the flag, at a offset no register
   uses, so a check can assert that DMA was revoked after the card was told to
   stop and not before. */
#define MODEL_BUSMASTER_OFF 0x40000000u

void pci_disable_bus_master(pci_device_t *dev)
{
    (void)dev;
    pci_master_enabled = 0;
    model_record(MODEL_BUSMASTER_OFF, 0);
}

void pci_enable_interrupts(pci_device_t *dev)
{
    (void)dev;
}

/* The mapping the driver asks for. A test can refuse it to reach the driver's
 * refusal. */
static int map_refused;
static unsigned map_size_asked;

unsigned int map_mmio(unsigned int phys, unsigned int size)
{
    map_size_asked = size;
    return map_refused ? 0 : phys;
}

static unsigned char isr_vector_registered;
static int isr_handler_registered;

void register_isr_handler(uint8_t vector, isr_handler_t handler, const char *name)
{
    isr_vector_registered = vector;
    isr_handler_registered = handler != 0;
}

unsigned char net_mac[6];

/* ------------------------------------------------------------------ */
/* The model of the chip.                                              */
/* ------------------------------------------------------------------ */

/* Registers are 32 bits and the highest one the driver touches is RAH0, so
   the file is indexed by dword and sized to hold it. */
#define MODEL_REGS ((E1000_RAH0 / 4) + 2)

static unsigned model_reg[MODEL_REGS];

/* Every register access, so the absent-card check can assert there were
   none. */
static unsigned model_accesses;

/* What the model does out of the ordinary, so a test can break one thing at a
 * time and watch the driver refuse, not hang. */
static int model_stuck_reset;  /* CTRL.RST never clears */
static int model_stuck_eeprom; /* EERD never reports done */
static int model_swallow_tx;   /* the transmit engine never runs */
static int model_dead_window;  /* every read answers all ones */
/* When nonzero, the transmit engine takes this many delays to finish what it
   accepted. It is what gives `stop`'s drain something to wait for: with the
   card completing synchronously, a driver that never drained would look the
   same as one that did. */
static unsigned model_tx_completes_after;
static unsigned short model_eeprom[64];

/* The recorded write sequence. */
struct wr {
    unsigned off;
    unsigned value;
};

#define MODEL_LOG_MAX 512
static struct wr model_log[MODEL_LOG_MAX];
static unsigned model_log_len;

static void model_record(unsigned off, unsigned value)
{
    if (model_log_len < MODEL_LOG_MAX) {
        model_log[model_log_len].off = off;
        model_log[model_log_len].value = value;
    }
    model_log_len++;
}

/* The chip's transmit engine, defined once the driver's rings are in scope. */
static void model_tx_drain(void);

/* The driver's own record for a card, which this model does not look
 * inside. */
struct e1000;

static unsigned m_r32(struct e1000 *e, unsigned off)
{
    (void)e; /* the model presents one card */
    model_accesses++;
    if (model_dead_window) {
        return 0xffffffffu;
    }
    if (off == E1000_CTRL) {
        /* The reset bit clears itself unless the test holds it. */
        if (!model_stuck_reset) {
            model_reg[off / 4] &= ~E1000_CTRL_RST;
        }
        return model_reg[off / 4];
    }
    if (off == E1000_ICR) {
        unsigned v = model_reg[off / 4];

        model_reg[off / 4] = 0; /* read to clear */
        return v;
    }
    return model_reg[off / 4];
}

static void m_w32(struct e1000 *e, unsigned off, unsigned value)
{
    (void)e;
    model_accesses++;
    model_record(off, value);
    model_reg[off / 4] = value;

    if (off == E1000_EERD && (value & E1000_EERD_START)) {
        unsigned word = (value >> E1000_EERD_ADDR_SHIFT) & 0xffu;

        if (model_stuck_eeprom) {
            model_reg[off / 4] = value & ~E1000_EERD_DONE;
        } else {
            model_reg[off / 4] =
                E1000_EERD_DONE | ((unsigned)model_eeprom[word & 63] << E1000_EERD_DATA_SHIFT);
        }
        return;
    }
    if (off == E1000_IMC) {
        /* Interrupt mask clear takes bits out of the mask register. */
        model_reg[E1000_IMS / 4] &= ~value;
        return;
    }
    if (off == E1000_TDT) {
        model_tx_drain();
        return;
    }
}

/* A write through the card's I/O window. Recorded in the same log as the
   memory writes, with its offset marked, so a check can assert that the reset
   went this way and not the other. */
#define MODEL_IO_MARK 0x80000000u

static void m_wio(struct e1000 *e, unsigned off, unsigned value)
{
    (void)e;
    model_accesses++;
    model_record(off | MODEL_IO_MARK, value);
    model_reg[off / 4] = value;
}

static void model_tx_finish(void);

static void m_udelay(unsigned usec)
{
    (void)usec;
    if (model_tx_completes_after > 0) {
        model_tx_completes_after--;
        if (model_tx_completes_after == 0) {
            model_tx_finish();
        }
    }
}

/* ------------------------------------------------------------------ */

/* Two of the kernel's own, which the driver reaches and this test does not
 * build: a fill, and the nth card of an identity on the bus. */
void *k_memset(void *ptr, int c, size_t n)
{
    unsigned char *q = (unsigned char *)ptr;

    while (n-- > 0) {
        *q++ = (unsigned char)c;
    }
    return ptr;
}

void *k_memcpy(void *dest, const void *src, size_t n)
{
    unsigned char *d = (unsigned char *)dest;
    const unsigned char *s = (const unsigned char *)src;
    size_t i;

    for (i = 0; i < n; i++) {
        d[i] = s[i];
    }
    return dest;
}

bool pci_find_nth_by_id(uint16_t vendor, uint16_t device, int nth, pci_device_t *out)
{
    return nth == 0 ? pci_find_device_by_id(vendor, device, out) : false;
}

/*
 * The identity-mapped frames the card's rings and buffers live in. On the
 * host they are pages of this program's memory, and the address the model
 * reads back is the address the driver wrote, which is what the checks
 * compare.
 */
#define FRAME_COUNT 64
static unsigned char frames[FRAME_COUNT][4096] __attribute__((aligned(4096)));
static unsigned frames_taken;

void *alloc_kernel_frame(void)
{
    if (frames_taken >= FRAME_COUNT) {
        return 0;
    }
    return &frames[frames_taken++][0];
}

void free_kernel_frame(void *frame)
{
    (void)frame;
}

#include "network/driver/e1000.c"

static int fails;

/* Declared, not included: the kernel's kint.h and the host's headers
 * disagree about uintptr_t, so this test pulls in no host header and names the
 * one function it needs to say which check failed. */
extern int printf(const char *fmt, ...);

#define CHECK(cond, what)                   \
    do {                                    \
        if (!(cond)) {                      \
            fails++;                        \
            printf("  FAIL: %s\n", (what)); \
        }                                   \
    } while (0)

static const struct e1000_io model_io = {m_r32, m_w32, m_wio, m_udelay};

/* The model's own transmit head, which is the index the card would be
   fetching from. The driver writes TDH once at setup and never again. */
static unsigned model_tdh;
static unsigned model_tx_sent;
static unsigned char model_tx_last[E1000_TX_BUF_SIZE];
static unsigned model_tx_last_len;

/**
 * Send every transmit descriptor between the model's head and the driver's
 * tail, and hand each one back.
 *
 * Writing the tail is what starts this on real hardware, so the model does it
 * from the tail write: without it the driver would wait for a done bit
 * nothing ever sets, which is exactly what the stall case tests. The frame
 * bytes are read out of the driver's own buffer rather than through the
 * descriptor's address, because that address is a kernel 32-bit value and
 * this test runs on a host whose pointers may be wider.
 */
/** Give back every descriptor the engine has taken but not yet finished. */
static void model_tx_finish(void)
{
    unsigned n;

    for (n = 0; n < E1000_RING_SIZE; n++) {
        e1000s[0].tx_ring[n].upper |= E1000_TXD_STAT_DD;
    }
}

static void model_tx_drain(void)
{
    unsigned tdt = model_reg[E1000_TDT / 4] % E1000_RING_SIZE;
    unsigned guard = 0;

    if (model_swallow_tx) {
        return;
    }
    while (model_tdh != tdt && guard < E1000_RING_SIZE * 2) {
        volatile struct e1000_tx_desc *d = &e1000s[0].tx_ring[model_tdh];
        unsigned len = d->lower & 0xffffu;
        unsigned i;

        model_tx_last_len = len;
        for (i = 0; i < len && i < sizeof(model_tx_last); i++) {
            model_tx_last[i] = e1000s[0].tx_buf[model_tdh][i];
        }
        if (model_tx_completes_after == 0) {
            d->upper |= E1000_TXD_STAT_DD;
        }
        model_tdh = (model_tdh + 1) % E1000_RING_SIZE;
        model_tx_sent++;
        guard++;
    }
    model_reg[E1000_TDH / 4] = model_tdh;
}

/* The model's own receive head, the descriptor it would fill next. */
static unsigned model_rdh;

/**
 * Put one frame into the receive ring the way the card would, including
 * refusing to when the ring is full.
 *
 * The card owns the descriptors from its head up to the driver's tail, and
 * stops when the two meet. Honouring that is what makes this a model rather
 * than a device that writes wherever it likes: a driver that stopped
 * returning descriptors would otherwise still be fed frames forever, and the
 * ring-wrap check would pass for a driver that never wrapped.
 *
 * The bytes go into the buffer the driver gave the descriptor, and the
 * length, status, and error bytes are written last, because the done bit is
 * what tells the driver the rest is readable.
 *
 * @return 1 when the frame was placed, 0 when the card had nowhere to put it.
 */
static int model_deliver(const unsigned char *data, unsigned len, unsigned char status,
                         unsigned char errors)
{
    volatile struct e1000_rx_desc *d = &e1000s[0].rx_ring[model_rdh];
    unsigned i;

    if (model_rdh == model_reg[E1000_RDT / 4] % E1000_RING_SIZE) {
        return 0; /* the card has caught up with the driver's tail */
    }
    for (i = 0; i < len && i < E1000_RX_BUF_SIZE; i++) {
        e1000s[0].rx_buf[model_rdh][i] = data[i];
    }
    d->len_csum = len & 0xffffu;
    d->sta_err = (unsigned)status | ((unsigned)errors << 8);
    model_rdh = (model_rdh + 1) % E1000_RING_SIZE;
    model_reg[E1000_RDH / 4] = model_rdh;
    model_reg[E1000_ICR / 4] |= E1000_INT_RXT0;
    return 1;
}

/** Put the model and the driver back to a cold-power state. */
static void model_reset(void)
{
    unsigned i;

    for (i = 0; i < MODEL_REGS; i++) {
        model_reg[i] = 0;
    }
    for (i = 0; i < 64; i++) {
        model_eeprom[i] = 0;
    }
    model_log_len = 0;
    model_accesses = 0;
    model_stuck_reset = 0;
    model_stuck_eeprom = 0;
    model_swallow_tx = 0;
    model_tx_completes_after = 0;
    pci_irq_line = 11;
    model_dead_window = 0;
    model_tdh = 0;
    model_rdh = 0;
    e1000s[0].rx_discarding = 0;
    model_tx_sent = 0;
    model_tx_last_len = 0;
    map_refused = 0;
    map_size_asked = 0;
    isr_vector_registered = 0;
    isr_handler_registered = 0;
    pci_present = 1;
    pci_master_enabled = 0;
    pci_memory_enabled = 0;
    note_rx_errors = 0;
    note_rx_drops = 0;
    rx_calls = 0;
    rx_last_len = 0;
    e1000s[0].up = 0;
    /* The window pointer and the PCI record are part of a cold-power state
       and are what `e1000_stop` reads to decide whether a register may be
       written at all. A reset that left them set would be a driver that
       remembers a card the machine no longer has. */
    e1000s[0].mmio = 0;
    e1000s[0].pci.vendor_id = 0;
    e1000s[0].pci.device_id = 0;
    e1000s[0].rx_cur = 0;
    e1000s[0].tx_head = 0;
    e1000s[0].rx_packets = 0;
    e1000s[0].rx_errors = 0;
    e1000s[0].rx_dropped = 0;
    e1000s[0].tx_packets = 0;
    e1000s[0].tx_stalls = 0;
    io = &model_io;
    for (i = 0; i < 6; i++) {
        e1000s[0].mac[i] = 0;
    }

    /* Every register the driver reads is seeded with a value that is wrong
     * for us. A driver that skips a write, or preserves bits it should clear,
     * then fails a check it would pass against a register file of zeroes. */
    for (i = 0; i < MODEL_REGS; i++) {
        model_reg[i] = 0xa5a5a5a5u;
    }
    model_reg[E1000_CTRL / 4] = E1000_CTRL_VME; /* VLAN mode left on */
    model_reg[E1000_ICR / 4] = 0;
    model_reg[E1000_EERD / 4] = 0;

    /* The station address the card presents in its register pair, with the
     * valid bit set: this is the QEMU case and the one the driver prefers. */
    model_reg[E1000_RAL0 / 4] = 0x00125452u; /* 52 54 12 00 */
    model_reg[E1000_RAH0 / 4] = E1000_RAH_AV | 0x00005634u;

    /* A different address in the EEPROM, so a driver that reads the wrong
     * source is visible rather than merely working. */
    model_eeprom[0] = 0x5452;
    model_eeprom[1] = 0xbbaa;
    model_eeprom[2] = 0xddcc;
}

/** The index of the first write to `off` at or after `from`, or -1. */
static int find_write(unsigned from, unsigned off)
{
    unsigned i;

    for (i = from; i < model_log_len && i < MODEL_LOG_MAX; i++) {
        if (model_log[i].off == off) {
            return (int)i;
        }
    }
    return -1;
}

/** The index of the first write to `off` with every bit of `bits` set. */
static int find_write_bits(unsigned from, unsigned off, unsigned bits)
{
    unsigned i;

    for (i = from; i < model_log_len && i < MODEL_LOG_MAX; i++) {
        if (model_log[i].off == off && (model_log[i].value & bits) == bits) {
            return (int)i;
        }
    }
    return -1;
}

/* The interface this model's card is brought up for. */
static struct netdev model_iface;

int main(void)
{
    int a, b, c;
    unsigned i;
    unsigned char frame[1600];

    for (i = 0; i < sizeof(frame); i++) {
        frame[i] = (unsigned char)(i & 0xff);
    }

    /* ---- the bring-up completes on a healthy model ---- */
    model_reset();
    {
        int rc = e1000_init(&model_iface, 0);

        if (rc != NETDEV_UP) {
            printf("  init refused: %s\n", e1000_err);
        }
        CHECK(rc == NETDEV_UP, "the bring-up completes");
    }
    CHECK(e1000s[0].up == 1, "the driver reports itself up");
    CHECK(pci_memory_enabled && pci_master_enabled, "decoding and bus mastering are enabled");
    CHECK(map_size_asked == E1000_BAR_WINDOW, "the whole declared window is mapped");
    CHECK(isr_handler_registered && isr_vector_registered == 32 + 11,
          "the handler is registered on the card's own line");
    CHECK(e1000s[0].mac[0] == 0x52 && e1000s[0].mac[1] == 0x54 && e1000s[0].mac[2] == 0x12 &&
              e1000s[0].mac[3] == 0x00 && e1000s[0].mac[4] == 0x34 && e1000s[0].mac[5] == 0x56,
          "the station address came from the register pair");

    /* ---- the bring-up order ---- */
    a = find_write_bits(0, E1000_IMC, 0xffffffffu);
    b = find_write_bits(0, E1000_CTRL | MODEL_IO_MARK, E1000_CTRL_RST);
    CHECK(a >= 0 && b >= 0 && a < b, "interrupts are masked before the reset");
    CHECK(b >= 0, "the reset goes through the card's I/O window, not its memory window, "
                  "which is what the erratum asks for");
    CHECK(find_write_bits(0, E1000_CTRL, E1000_CTRL_RST) < 0,
          "and never through the memory window");
    {
        int rctl0 = find_write(0, E1000_RCTL);
        int tctl0 = find_write(0, E1000_TCTL);

        CHECK(rctl0 >= 0 && model_log[rctl0].value == 0 && rctl0 < b,
              "the receiver is turned off before the reset, in case the card arrived running");
        CHECK(tctl0 >= 0 && model_log[tctl0].value == 0 && tctl0 < b, "and the transmitter too");
    }

    c = find_write_bits(0, E1000_RCTL, E1000_RCTL_EN);
    CHECK(c >= 0, "the receiver is enabled");
    CHECK(find_write(0, E1000_RDBAL) >= 0 && find_write(0, E1000_RDBAL) < c,
          "the receive ring base is programmed before the receiver starts");
    CHECK(find_write(0, E1000_RDLEN) >= 0 && find_write(0, E1000_RDLEN) < c,
          "the receive ring length is programmed before the receiver starts");
    CHECK(find_write(0, E1000_RDT) >= 0 && find_write(0, E1000_RDT) < c,
          "the receive tail is programmed before the receiver starts");
    a = find_write_bits(0, E1000_TCTL, E1000_TCTL_EN);
    CHECK(a >= 0, "the transmitter is enabled");
    CHECK(find_write(0, E1000_TDBAL) >= 0 && find_write(0, E1000_TDBAL) < a,
          "the transmit ring base is programmed before the transmitter starts");
    CHECK(find_write(0, E1000_TDLEN) >= 0 && find_write(0, E1000_TDLEN) < a,
          "the transmit ring length is programmed before the transmitter starts");
    CHECK(model_reg[E1000_RDLEN / 4] == E1000_RING_BYTES &&
              model_reg[E1000_TDLEN / 4] == E1000_RING_BYTES,
          "both ring lengths are the byte count the card expects");
    CHECK(model_reg[E1000_RDT / 4] == E1000_RING_SIZE - 1,
          "the receive tail leaves one descriptor out of the card's reach");
    CHECK((model_reg[E1000_CTRL / 4] & E1000_CTRL_VME) == 0, "VLAN mode is turned off");
    CHECK((model_reg[E1000_CTRL / 4] & (E1000_CTRL_SLU | E1000_CTRL_ASDE)) ==
              (E1000_CTRL_SLU | E1000_CTRL_ASDE),
          "the link is asked for and the speed left to the card");
    CHECK((model_reg[E1000_RCTL / 4] & E1000_RCTL_SECRC) != 0,
          "the frame check sequence is stripped, so a length is a frame length");
    CHECK((model_reg[E1000_RCTL / 4] & E1000_RCTL_BAM) != 0,
          "broadcast is accepted, which ARP and DHCP need");
    CHECK((model_reg[E1000_RCTL / 4] & (E1000_RCTL_UPE | E1000_RCTL_MPE)) == 0,
          "the card is not left promiscuous");
    {
        int cleared = 1;

        for (i = 0; i < E1000_MTA_ENTRIES; i++) {
            if (model_reg[(E1000_MTA + i * 4) / 4] != 0) {
                cleared = 0;
            }
        }
        CHECK(cleared, "every multicast table entry is cleared");
    }

    /* ---- the station address from the EEPROM, and written back ---- */
    model_reset();
    model_reg[E1000_RAH0 / 4] = 0; /* no valid address in the pair */
    CHECK(e1000_init(&model_iface, 0) == NETDEV_UP,
          "the bring-up completes with an empty address pair");
    CHECK(e1000s[0].mac[0] == 0x52 && e1000s[0].mac[1] == 0x54 && e1000s[0].mac[2] == 0xaa &&
              e1000s[0].mac[3] == 0xbb && e1000s[0].mac[4] == 0xcc && e1000s[0].mac[5] == 0xdd,
          "the station address came from the EEPROM when the pair had none");
    CHECK((model_reg[E1000_RAH0 / 4] & E1000_RAH_AV) != 0 &&
              (model_reg[E1000_RAH0 / 4] & 0xffffu) == 0xddccu &&
              model_reg[E1000_RAL0 / 4] == 0xbbaa5452u,
          "the address read from the EEPROM is published so the card can filter to it");

    /* ---- the bounded failures ---- */
    model_reset();
    model_stuck_reset = 1;
    CHECK(e1000_init(&model_iface, 0) == NETDEV_FAILED, "a reset that never completes is refused");
    CHECK(!e1000s[0].up, "a refused bring-up does not report itself up");

    model_reset();
    model_reg[E1000_RAH0 / 4] = 0;
    model_stuck_eeprom = 1;
    CHECK(e1000_init(&model_iface, 0) == NETDEV_FAILED, "an EEPROM that never answers is refused");

    model_reset();
    model_reg[E1000_RAH0 / 4] = 0;
    model_eeprom[0] = 0;
    model_eeprom[1] = 0;
    model_eeprom[2] = 0;
    CHECK(e1000_init(&model_iface, 0) == NETDEV_FAILED, "an all-zero station address is refused");

    model_reset();
    model_reg[E1000_RAH0 / 4] = 0;
    model_eeprom[0] = 0x0001; /* the group bit, which no station address has */
    CHECK(e1000_init(&model_iface, 0) == NETDEV_FAILED, "a group station address is refused");

    model_reset();
    model_dead_window = 1;
    CHECK(e1000_init(&model_iface, 0) == NETDEV_FAILED,
          "a window that reads back all ones is refused");

    model_reset();
    map_refused = 1;
    CHECK(e1000_init(&model_iface, 0) == NETDEV_FAILED, "a window that will not map is refused");

    /* ---- a card that is not in the machine is not touched ---- */
    model_reset();
    pci_present = 0;
    CHECK(e1000_init(&model_iface, 0) == NETDEV_ABSENT, "an absent card reports itself absent");
    CHECK(model_accesses == 0, "an absent card costs not one register access");
    CHECK(!pci_memory_enabled && !pci_master_enabled,
          "an absent card leaves the PCI command word alone");

    /* ---- receive: an ordinary frame ---- */
    model_reset();
    CHECK(e1000_init(&model_iface, 0) == NETDEV_UP,
          "the bring-up completes before the receive checks");
    CHECK(model_deliver(frame, 100, E1000_RXD_STAT_DD | E1000_RXD_STAT_EOP, 0),
          "the card has a descriptor to put the frame in");
    e1000_irq(0);
    CHECK(rx_calls == 1 && rx_last_len == 100, "a whole frame reaches the stack with its length");
    CHECK(rx_last[0] == 0 && rx_last[99] == 99, "the bytes handed up are the bytes received");
    CHECK(e1000s[0].rx_packets == 1, "the frame is counted");
    CHECK(model_reg[E1000_RDT / 4] == 0, "the tail returns the descriptor that was read");
    CHECK((e1000s[0].rx_ring[0].sta_err & 0xffu) == 0,
          "the descriptor's status is cleared before it goes back");

    /* ---- receive: each of the four checks fails on its own ---- */
    model_reset();
    CHECK(e1000_init(&model_iface, 0) == NETDEV_UP,
          "the bring-up completes before the refusal checks");
    (void)model_deliver(frame, 100, E1000_RXD_STAT_DD | E1000_RXD_STAT_EOP, 0x80);
    e1000_irq(0);
    CHECK(rx_calls == 0 && e1000s[0].rx_errors == 1,
          "a descriptor reporting an error is refused even though it is done and complete");
    CHECK(note_rx_errors == 1 && note_rx_drops == 0,
          "a hardware error is reported to the interface as an error, not a drop");

    model_reset();
    CHECK(e1000_init(&model_iface, 0) == NETDEV_UP, "the bring-up completes");
    /* A split frame is two descriptors: one without end-of-packet and one
       with it. A driver that judged each on its own would refuse the first
       and hand the second up as a whole frame, which is the second half of
       something presented as all of it. Both halves must be refused. */
    model_deliver(frame, 100, E1000_RXD_STAT_DD, 0);
    model_deliver(frame + 100, 60, E1000_RXD_STAT_DD | E1000_RXD_STAT_EOP, 0);
    e1000_irq(0);
    CHECK(rx_calls == 0 && e1000s[0].rx_dropped == 2,
          "a frame split across descriptors is refused");
    CHECK(note_rx_drops == 2 && note_rx_errors == 0,
          "a split frame is reported to the interface as a drop, not an error");

    /* And the receiver comes back: the frame after a split one is delivered
       normally, so the discard ends where the split frame ends. */
    model_deliver(frame, 80, E1000_RXD_STAT_DD | E1000_RXD_STAT_EOP, 0);
    e1000_irq(0);
    CHECK(rx_calls == 1 && rx_last_len == 80,
          "the frame after a split one is delivered, so the discard ended with it");

    model_reset();
    CHECK(e1000_init(&model_iface, 0) == NETDEV_UP, "the bring-up completes");
    (void)model_deliver(frame, 13, E1000_RXD_STAT_DD | E1000_RXD_STAT_EOP, 0);
    e1000_irq(0);
    CHECK(rx_calls == 0 && e1000s[0].rx_dropped == 1,
          "a frame too short to hold an Ethernet header is refused");

    model_reset();
    CHECK(e1000_init(&model_iface, 0) == NETDEV_UP, "the bring-up completes");
    (void)model_deliver(frame, 100, E1000_RXD_STAT_DD | E1000_RXD_STAT_EOP, 0);
    e1000s[0].rx_ring[0].len_csum = E1000_RX_BUF_SIZE + 1;
    e1000_irq(0);
    CHECK(rx_calls == 0 && e1000s[0].rx_dropped == 1,
          "a length past the end of the buffer is refused, not trusted");

    /* ---- receive: the ring wraps ---- */
    model_reset();
    CHECK(e1000_init(&model_iface, 0) == NETDEV_UP, "the bring-up completes before the wrap check");
    {
        int placed = 1;

        for (i = 0; i < E1000_RING_SIZE + 5 && placed; i++) {
            placed = model_deliver(frame, 64, E1000_RXD_STAT_DD | E1000_RXD_STAT_EOP, 0);
            e1000_irq(0);
        }
        CHECK(placed, "the driver kept returning descriptors, so the card never ran out");
    }
    CHECK(rx_calls == E1000_RING_SIZE + 5, "every frame across a ring wrap is delivered");
    CHECK(e1000s[0].rx_cur == (E1000_RING_SIZE + 5) % E1000_RING_SIZE,
          "the read index wrapped rather than ran off the ring");

    /* ---- transmit: the descriptor contract ---- */
    model_reset();
    CHECK(e1000_init(&model_iface, 0) == NETDEV_UP,
          "the bring-up completes before the transmit checks");
    CHECK(e1000_tx_frame(&e1000s[0], frame, 200) == NETDEV_TX_OK,
          "a frame that is sent reports that it was");
    CHECK(model_tx_sent == 1 && model_tx_last_len == 200, "a frame is sent with its own length");
    CHECK(e1000s[0].tx_packets == 1, "the frame is counted");
    {
        unsigned lower = e1000s[0].tx_ring[0].lower;
        unsigned char cmd = (unsigned char)(lower >> 24);

        CHECK((lower & 0xffffu) == 200, "the descriptor carries the frame's length");
        CHECK((cmd & E1000_TXD_CMD_EOP) != 0, "the descriptor is marked end of packet");
        CHECK((cmd & E1000_TXD_CMD_IFCS) != 0, "the card is asked to append the check sequence");
        CHECK((cmd & E1000_TXD_CMD_RS) != 0,
              "the card is asked to report status, without which no descriptor is ever free");
        CHECK(e1000s[0].tx_ring[0].addr_lo == (unsigned)(uintptr_t)e1000s[0].tx_buf[0],
              "the descriptor points at the buffer the frame was copied into");
    }

    /* ---- transmit: a short frame is padded ---- */
    model_reset();
    CHECK(e1000_init(&model_iface, 0) == NETDEV_UP, "the bring-up completes");
    e1000_tx_frame(&e1000s[0], frame, 20);
    CHECK(model_tx_last_len == 60, "a short frame is padded to the Ethernet minimum");
    CHECK(model_tx_last[59] == 0, "the padding is zeroed, not whatever the buffer held");

    /* ---- transmit: an over-length frame is refused ---- */
    model_reset();
    CHECK(e1000_init(&model_iface, 0) == NETDEV_UP, "the bring-up completes");
    CHECK(e1000_tx_frame(&e1000s[0], frame, E1000_FRAME_MAX + 1) == NETDEV_TX_DROPPED,
          "an over-length frame reports that it was dropped");
    CHECK(model_tx_sent == 0 && e1000s[0].tx_packets == 0,
          "a frame larger than the card's own maximum is refused");
    CHECK(e1000s[0].tx_head == 0, "a refused frame consumes no descriptor");

    /* ---- transmit: a card that never finishes stalls, bounded ---- */
    model_reset();
    CHECK(e1000_init(&model_iface, 0) == NETDEV_UP, "the bring-up completes");
    model_swallow_tx = 1;
    {
        int refusals = 0;

        for (i = 0; i < E1000_RING_SIZE + 2; i++) {
            if (e1000_tx_frame(&e1000s[0], frame, 64) == NETDEV_TX_DROPPED) {
                refusals++;
            }
        }
        CHECK(refusals > 0, "a frame the stalled ring could not take reports that it was dropped");
    }
    CHECK(e1000s[0].tx_stalls > 0,
          "a ring the card never drains is counted as a stall, not a hang");
    CHECK(e1000s[0].tx_packets == E1000_RING_SIZE - 1,
          "one descriptor is left unsubmitted, so a full ring is not an empty one");
    /* The whole point of that spare descriptor. The card reads head equal to
       tail as nothing to do, so a driver that filled every entry would wrap
       the tail onto the head and lose the entire outstanding queue. */
    CHECK(model_reg[E1000_TDT / 4] != model_reg[E1000_TDH / 4],
          "a full ring never looks empty to the card");

    /* ---- a driver that is not up does nothing ---- */
    model_reset();
    CHECK(e1000_tx_frame(&e1000s[0], frame, 64) == NETDEV_TX_DROPPED,
          "a driver that never came up refuses the frame rather than swallowing it");
    e1000_irq(0);
    CHECK(e1000s[0].tx_packets == 0 && rx_calls == 0,
          "a driver that never came up neither sends nor receives");

    /* ---- stop: the order, and what it leaves ---- */
    model_reset();
    CHECK(e1000_init(&model_iface, 0) == NETDEV_UP,
          "the bring-up completes before the stop checks");
    {
        unsigned mark = model_log_len;
        int rctl, tctl, imc;

        e1000_stop(&e1000s[0]);
        rctl = find_write(mark, E1000_RCTL);
        tctl = find_write(mark, E1000_TCTL);
        imc = find_write_bits(mark, E1000_IMC, 0xffffffffu);
        CHECK(rctl >= 0 && model_log[rctl].value == 0, "the receiver is turned off");
        CHECK(tctl >= 0 && model_log[tctl].value == 0, "the transmitter is turned off");
        CHECK(imc >= 0, "every interrupt is masked");
        CHECK(rctl < imc && tctl < imc,
              "the card is told to stop while its interrupts are still its own");
        {
            int bm = find_write(mark, MODEL_BUSMASTER_OFF);

            CHECK(bm >= 0, "bus mastering is revoked, which is the write that ends DMA whatever "
                           "the card believes");
            CHECK(bm > rctl && bm > tctl && bm > imc,
                  "and revoked last, after the card was told to stop through registers "
                  "it can still be reached by");
        }
        CHECK(!pci_master_enabled, "the card is left unable to master the bus");
        CHECK(!e1000s[0].up, "a stopped driver no longer reports itself up");
    }

    /* Twice. No current path calls it twice, but the interface says it must
       be safe to, and a second call must write nothing new: the card has
       already been told, and the write log is what says so. */
    {
        unsigned mark = model_log_len;

        e1000_stop(&e1000s[0]);
        CHECK(!e1000s[0].up, "stopping a stopped card changes nothing");
        CHECK(model_log_len > mark, "a second stop is still a stop");
    }

    /* On a card that never finished starting, which is how the probe calls it
       after a failed bring-up. The failure is arranged for a point AFTER the
       driver has enabled bus mastering, because a failure before that leaves
       nothing to revoke and would pass whatever stop did. The station address
       is read after the rings are programmed and the bus is taken, so an
       address the card will not report fails there. */
    model_reset();
    model_stuck_reset = 1;
    CHECK(e1000_init(&model_iface, 0) == NETDEV_FAILED,
          "a bring-up that fails at the reset is refused");
    CHECK(!pci_master_enabled, "and never took the bus in the first place");

    model_reset();
    model_swallow_tx = 1;
    CHECK(e1000_init(&model_iface, 0) == NETDEV_UP, "a card that starts takes the bus");
    CHECK(pci_master_enabled, "and is mastering it");
    {
        unsigned mark = model_log_len;

        e1000_stop(&e1000s[0]);
        CHECK(find_write(mark, MODEL_BUSMASTER_OFF) >= 0 && !pci_master_enabled,
              "a card that was started and then stopped is left unable to master the bus");
    }

    /* And on a machine that has no such card at all, where init wrote
       nothing: stop must write nothing either. */
    model_reset();
    pci_present = 0;
    CHECK(e1000_init(&model_iface, 0) == NETDEV_ABSENT, "the card is absent");
    {
        unsigned mark = model_accesses;

        e1000_stop(&e1000s[0]);
        CHECK(model_accesses == mark, "stopping an absent card costs no register access");
    }

    /* ---- stop waits for what the card already took ---- */
    model_reset();
    CHECK(e1000_init(&model_iface, 0) == NETDEV_UP,
          "the bring-up completes before the drain check");
    model_tx_completes_after = 3;
    CHECK(e1000_tx_frame(&e1000s[0], frame, 200) == NETDEV_TX_OK,
          "a frame is accepted by a slow card");
    CHECK(e1000s[0].tx_head != e1000s[0].tx_tail,
          "and is still outstanding, since the card is slow");
    e1000_stop(&e1000s[0]);
    CHECK(e1000s[0].tx_head == e1000s[0].tx_tail,
          "stop waits for what the card accepted before silencing it, so the last log "
          "line a remote reader is waiting for is sent and not discarded");

    /* A card that never finishes costs the wait and no more. */
    model_reset();
    CHECK(e1000_init(&model_iface, 0) == NETDEV_UP, "the bring-up completes");
    model_swallow_tx = 1;
    CHECK(e1000_tx_frame(&e1000s[0], frame, 200) == NETDEV_TX_OK, "a frame is accepted");
    e1000_stop(&e1000s[0]);
    CHECK(!pci_master_enabled, "a card that never finishes is still stopped, bounded");

    /* ---- a line this kernel cannot deliver ---- */
    model_reset();
    pci_irq_line = 20;
    CHECK(e1000_init(&model_iface, 0) == NETDEV_FAILED,
          "an interrupt line outside the sixteen a PIC can deliver is refused");
    CHECK(!isr_handler_registered,
          "and no handler is installed, which for a large enough line would land on an "
          "exception vector");

    model_reset();
    pci_irq_line = 15;
    CHECK(e1000_init(&model_iface, 0) == NETDEV_UP, "the last line a PIC can deliver is accepted");

    /* ---- the link ---- */
    model_reset();
    CHECK(e1000_init(&model_iface, 0) == NETDEV_UP,
          "the bring-up completes before the link checks");
    {
        struct netdev_link link;

        model_reg[E1000_STATUS / 4] = E1000_STATUS_LU | E1000_STATUS_FD | (2u << 6);
        CHECK(e1000_link(&e1000s[0], &link) == 1, "a card whose registers can be read answers");
        CHECK(link.up && link.mbps == 1000 && link.full_duplex,
              "a gigabit full-duplex link is reported as one");

        model_reg[E1000_STATUS / 4] = E1000_STATUS_LU | (1u << 6);
        CHECK(e1000_link(&e1000s[0], &link) && link.mbps == 100 && !link.full_duplex,
              "a hundred-megabit half-duplex link is reported as one");

        model_reg[E1000_STATUS / 4] = E1000_STATUS_LU;
        CHECK(e1000_link(&e1000s[0], &link) && link.mbps == 10, "ten megabit is reported as ten");

        /* Both 2 and 3 encode a gigabit, and a driver that read the field as
           a table would get the second of them wrong. */
        model_reg[E1000_STATUS / 4] = E1000_STATUS_LU | (3u << 6);
        CHECK(e1000_link(&e1000s[0], &link) && link.mbps == 1000,
              "the second encoding of a gigabit is a gigabit too");

        model_reg[E1000_STATUS / 4] = 0;
        CHECK(e1000_link(&e1000s[0], &link) == 1 && !link.up,
              "a card with no link says so, which is not the same as not saying");
    }

    /* A card that was never found cannot be asked. */
    model_reset();
    pci_present = 0;
    CHECK(e1000_init(&model_iface, 0) == NETDEV_ABSENT, "the card is absent");
    {
        struct netdev_link link;

        link.up = 42;
        CHECK(e1000_link(&e1000s[0], &link) == 0,
              "a card with no window reports that it cannot say");
        CHECK(link.up == 42, "and leaves the caller's answer untouched");
    }

    /* ---- the driver's table ---- */
    CHECK(e1000_netdev.tx == e1000_op_tx, "the table names the transmit path");
    CHECK(e1000_netdev.poll == 0, "the table leaves poll empty, because this card interrupts");
    CHECK(e1000_netdev.init == e1000_init, "the table names the bring-up");
    CHECK(e1000_netdev.error != 0, "the table names a diagnostic");
    CHECK(e1000_netdev.stop == e1000_op_stop, "the table names the way to silence the card");
    CHECK(e1000_netdev.link == e1000_op_link, "the table names the way to ask about the link");

    CHECK(lock_depth == 0 && !lock_ever_negative,
          "every interrupt bracket the driver took, it released");

    return fails > 255 ? 255 : fails;
}
