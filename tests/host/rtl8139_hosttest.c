/**
 * Drive repeated receive overflows through the interrupt handler of the
 * RTL8139 driver, rtl8139.c, and check that it drains and counts each.
 *
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 *
 * The registers give one complete frame and an overflow status for each
 * interrupt. A write to the interrupt status register clears the bits
 * written, and a write of CAPR releases the frame. Every frame must be
 * delivered and every overflow counted, with one warning for the boot. The
 * transmit row requires the descriptor write to come under net_lock and the
 * saved flags to reach net_unlock. PCI discovery and DMA are not modeled.
 */

#include "network/driver/rtl8139.h"
#include "pci.h"
#include "isr.h"
#include "klog.h"
#include "mm/heap.h"

extern int printf(const char *format, ...);

static struct netdev iface;
static uint8_t ring[RTL_RX_BUF_SIZE];
static uint16_t pending;       /* the interrupt sources set in RTL_ISR */
static uint16_t write_pointer; /* RTL_CBR: the card's write offset */
static int occupied;           /* the ring has a frame CAPR has not released */
static unsigned acknowledgements;
static unsigned releases;
static unsigned deliveries;
static unsigned bad_frames;
static unsigned warnings;
static unsigned overflow_warnings;
static unsigned tx_starts;
static unsigned bad_locks; /* descriptor writes and releases out of the lock */
static int lock_depth;     /* net_lock calls not yet released */
static int fails;

/** Return 1 when `text` begins with `prefix`; the host's string.h would
 * conflict with the kernel's types. */
static int starts_with(const char *text, const char *prefix)
{
    while (*prefix) {
        if (*text++ != *prefix++) {
            return 0;
        }
    }
    return 1;
}

/** Report each failed contract without stopping later checks. */
static void check(int condition, const char *reason)
{
    if (!condition) {
        printf("rtl8139-test: fail: %s\n", reason);
        fails++;
    }
}

/** Count every warning, and apart from them each one for a filled ring. */
void klog(int level, const char *subsystem, const char *format, ...)
{
    (void)subsystem;
    if (level == KLOG_WARN) {
        warnings++;
        if (starts_with(format, "rx: the ring filled; draining it")) {
            overflow_warnings++;
        }
    }
}

/** Read the command register's empty-buffer bit, set while the ring has no
 * frame; any other 8-bit register reads 0. */
uint8_t pci_io_read8(uint16_t port)
{
    return port == RTL_CHIPCMD && !occupied ? RTL_CMD_RXEMPTY : 0;
}

/** Read the pending sources from the interrupt status register and the
 * write pointer from CBR; any other 16-bit register reads 0. */
uint16_t pci_io_read16(uint16_t port)
{
    if (port == RTL_ISR) {
        return pending;
    }
    return port == RTL_CBR ? write_pointer : 0;
}

/** Read 0 from any 32-bit register; the rows read none. */
uint32_t pci_io_read32(uint16_t port)
{
    (void)port;
    return 0;
}

/** Ignore an 8-bit register write; the model starts with the ring set up. */
void pci_io_write8(uint16_t port, uint8_t value)
{
    (void)port;
    (void)value;
}

/**
 * Clear the bits written to the interrupt status register and count the
 * acknowledgement. A write of CAPR must release the whole frame, 16 bytes
 * before the write pointer, and leaves the ring empty.
 */
void pci_io_write16(uint16_t port, uint16_t value)
{
    if (port == RTL_ISR) {
        pending &= (uint16_t)~value;
        acknowledgements++;
    } else if (port == RTL_CAPR) {
        check(value == (uint16_t)(write_pointer - 16), "CAPR releases the complete frame");
        occupied = 0;
        releases++;
    }
}

/** Count a write of a transmit status register, which starts a transmit, and
 * count a bad lock unless the network lock is held once. */
void pci_io_write32(uint16_t port, uint32_t value)
{
    (void)value;
    if (port >= RTL_TSD0 && port < RTL_TSD0 + RTL_TX_NUM * 4) {
        tx_starts++;
        if (lock_depth != 1) {
            bad_locks++;
        }
    }
}

/** Take the network lock and return 0x200, the interrupt flag, as the saved
 * flags. */
uint32_t net_lock(void)
{
    lock_depth++;
    return 0x200;
}

/** Release the network lock; count a bad lock unless it is held once and
 * `flags` is what net_lock returned. */
void net_unlock(uint32_t flags)
{
    if (lock_depth != 1 || flags != 0x200) {
        bad_locks++;
    }
    lock_depth--;
}

/** Count a delivered frame, and count it bad unless it comes from `iface` and
 * is 60 bytes of 0x5a. */
void ethernet_rx(struct netdev *nd, const uint8_t *data, uint16_t len)
{
    unsigned i;

    deliveries++;
    if (nd != &iface || len != 60) {
        bad_frames++;
        return;
    }
    for (i = 0; i < len; i++) {
        if (data[i] != 0x5a) {
            bad_frames++;
            break;
        }
    }
}

/** Count a receive error on `nd`. */
void ethernet_note_rx_error(struct netdev *nd)
{
    nd->rx_errors++;
}

/** Count a dropped frame on `nd`. */
void ethernet_note_rx_drop(struct netdev *nd)
{
    nd->rx_dropped++;
}

/** Find no device; main sets up the driver's state itself. */
int pci_find_device_by_id(uint16_t vendor, uint16_t device, pci_device_t *out)
{
    (void)vendor;
    (void)device;
    (void)out;
    return 0;
}

/** Decode an I/O BAR without touching hardware. */
uint32_t pci_bar_address(uint32_t bar)
{
    return bar & ~3u;
}

/** Enable bus mastering: a no-op, since the model has no DMA. */
void pci_enable_bus_master(pci_device_t *dev)
{
    (void)dev;
}

/** Disable bus mastering: a no-op. */
void pci_disable_bus_master(pci_device_t *dev)
{
    (void)dev;
}

/** Register nothing; the rows call rtl8139_irq directly. */
void register_isr_handler(uint8_t vector, isr_handler_t handler, const char *name)
{
    (void)vector;
    (void)handler;
    (void)name;
}

/** Reserve nothing and return 0; main points rx_buf at the test's ring. */
uintptr_t mem_reserve(size_t size, bool align)
{
    (void)size;
    (void)align;
    return 0;
}

#include "network/driver/rtl8139.c"

/**
 * Raise 1024 interrupts, each with one frame and one of the two overflow
 * sources, then a receive error, then send a frame; return 0 when every row
 * passes.
 */
int main(void)
{
    unsigned i;

    rtl8139_iface = &iface;
    rx_buf = ring;
    for (i = 0; i < 1024; i++) {
        k_memset(ring + rx_ptr, 0x5a, 68);
        ring[rx_ptr] = RTL_ISR_ROK;
        ring[rx_ptr + 1] = 0;
        ring[rx_ptr + 2] = 64;
        ring[rx_ptr + 3] = 0;
        write_pointer = (uint16_t)((rx_ptr + 68) % RTL_RX_RING);
        pending = i % 2 ? RTL_ISR_RXOVW : RTL_ISR_FOVW;
        if (i % 3 == 0) {
            pending |= RTL_ISR_ROK;
        }
        occupied = 1;
        rtl8139_irq(0);
    }
    check(overflow_warnings == 1, "repeated overflows produce one warning per boot");
    check(warnings == 1, "overflow recovery produces no other warning");
    check(iface.rx_dropped == 1024, "every overflow is counted");
    check(deliveries == 1024 && bad_frames == 0, "every retained frame is delivered intact");
    check(releases == 1024 && acknowledgements == 1024, "every frame and source is released");
    check(iface.rx_errors == 0 && pending == 0 && !occupied, "valid frames need no reset");
    pending = RTL_ISR_RER;
    rtl8139_irq(0);
    check(warnings == 2 && iface.rx_errors == 1, "a receive error still reports its warning");
    check(rtl8139_tx(0, ring, 60) == NETDEV_TX_OK, "a transmit still completes");
    check(tx_starts == 1 && bad_locks == 0 && lock_depth == 0,
          "transmit starts under the saved interrupt flag and restores it");
    if (!fails) {
        printf("rtl8139-test: result: 1024 overflows counted and drained, one warning\n");
    }
    return fails ? 1 : 0;
}
