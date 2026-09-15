/**
 * Drive repeated receive overflows through the RTL8139 interrupt handler.
 *
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 *
 * Register reads supply one complete frame and an overflow status per entry.
 * Acknowledgement clears the status and CAPR releases the frame. Every frame
 * must be delivered and every overflow counted, with only one warning for the
 * boot. The transmit check requires the interrupt bracket to restore its flag.
 * PCI discovery and DMA are not modeled.
 */

#include "network/driver/rtl8139.h"
#include "pci.h"
#include "isr.h"
#include "klog.h"
#include "mm/heap.h"

extern int printf(const char *format, ...);

static struct netdev iface;
static uint8_t ring[RTL_RX_BUF_SIZE];
static uint16_t pending;
static uint16_t write_pointer;
static int occupied;
static unsigned acknowledgements;
static unsigned releases;
static unsigned deliveries;
static unsigned bad_frames;
static unsigned warnings;
static unsigned overflow_warnings;
static unsigned tx_starts;
static unsigned bad_locks;
static int lock_depth;
static int fails;

/** Match a diagnostic prefix without importing conflicting host types. */
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

/** Count warnings at the driver boundary, including unexpected messages. */
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

/** Supply only the receiver's empty flag. */
uint8_t inb(uint16_t port)
{
    return port == RTL_CHIPCMD && !occupied ? RTL_CMD_RXEMPTY : 0;
}

/** Supply the pending sources and the completed frame's end. */
uint16_t inw(uint16_t port)
{
    if (port == RTL_ISR) {
        return pending;
    }
    return port == RTL_CBR ? write_pointer : 0;
}

/** Discovery is unused, so no station-address register is supplied. */
uint32_t ind(uint16_t port)
{
    (void)port;
    return 0;
}

/** Accept unused control writes; this model starts with a configured ring. */
void outb(uint16_t port, uint8_t value)
{
    (void)port;
    (void)value;
}

/** Acknowledge status and release a frame only at its expected read pointer. */
void outw(uint16_t port, uint16_t value)
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

/** Record whether a transmit descriptor starts with interrupts bracketed. */
void outd(uint16_t port, uint32_t value)
{
    (void)value;
    if (port >= RTL_TSD0 && port < RTL_TSD0 + RTL_TX_NUM * 4) {
        tx_starts++;
        if (lock_depth != 1) {
            bad_locks++;
        }
    }
}

/** Return a distinguishable saved interrupt flag. */
uint32_t net_lock(void)
{
    lock_depth++;
    return 0x200;
}

/** Require the saved interrupt flag at release. */
void net_unlock(uint32_t flags)
{
    if (lock_depth != 1 || flags != 0x200) {
        bad_locks++;
    }
    lock_depth--;
}

/** Check the interface, length and bytes delivered from each retained frame. */
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

/** Preserve the interface's error counter. */
void ethernet_note_rx_error(struct netdev *nd)
{
    nd->rx_errors++;
}

/** Preserve the interface's drop counter. */
void ethernet_note_rx_drop(struct netdev *nd)
{
    nd->rx_dropped++;
}

/** Exclude PCI discovery from this receive-path model. */
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

/** No DMA engine exists in this model. */
void pci_enable_bus_master(pci_device_t *dev)
{
    (void)dev;
}

/** No DMA engine exists in this model. */
void pci_disable_bus_master(pci_device_t *dev)
{
    (void)dev;
}

/** The test calls the included interrupt handler directly. */
void register_isr_handler(uint8_t vector, isr_handler_t handler, const char *name)
{
    (void)vector;
    (void)handler;
    (void)name;
}

/** The test supplies its ring directly, avoiding a 32-bit DMA address. */
uintptr_t mem_reserve(size_t size, bool align)
{
    (void)size;
    (void)align;
    return 0;
}

#include "network/driver/rtl8139.c"

/** Check both overflow sources repeatedly, then an error and a transmit. */
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
