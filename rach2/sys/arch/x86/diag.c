/*
 * diag.c: the `rach2.diag` boot report.
 *
 * On a machine without a serial port a first bring-up sees only what
 * reaches the screen. These are the numbers that decide whether a boot is
 * worth continuing: the layout the loader chose, what memory is left after
 * the reservations, and the identity of the Ethernet controller, read from
 * the chip and not assumed from its PCI device id, because Realtek gives one
 * id to a family whose members need different bring-up sequences.
 *
 * Every line is a kernel log record, so one call puts it on the screen, on
 * the serial port and in the log: the screen is what a camera or a capture
 * card reads on a machine with no serial port, and the serial line is what
 * a harness reads under QEMU.
 */

#include "portio.h"
#include "../../klog.h"
#include "diag.h"
#include "../../kio.h"
#include "../../serial.h"
#include "../../module.h"
#include "../../pci.h"
#include "board.h"
#include "../../block/diskcmd.h"
#include "../../fs/krffs.h"
#include "../../lib/krandom.h"
#include "../../network/net.h"
#include "../../network/driver/netdev.h"
#include "../../mm/heap.h"
#include "../../mm/paging.h"

/* The Realtek gigabit family. The device id alone spans RTL8168B through H, so
 * the chip is told by its own XID, below. */
#define DIAG_REALTEK_VENDOR 0x10EC
#define DIAG_RTL8168_DEVICE 0x8168

/* TxConfig, whose top bits are the hardware revision Linux calls the XID.
 * The mask and shift are the reference driver's (`(TxConfig >> 20) & 0xfcf`),
 * and RTL8168GU has `(xid & 0x7cf) == 0x509`. */
#define DIAG_TXCONFIG        0x40
#define DIAG_XID_SHIFT       20
#define DIAG_XID_MASK        0xfcf
#define DIAG_XID_FAMILY_MASK 0x7cf
#define DIAG_XID_RTL8168GU   0x509
#define DIAG_MMIO_WINDOW     256

/* What the peek found, so the work can happen before the figures are read and
 * the printing after. */
#define DIAG_ETH_ABSENT    0
#define DIAG_ETH_NO_DECODE 1
#define DIAG_ETH_NO_WINDOW 2
#define DIAG_ETH_NO_MAP    3
#define DIAG_ETH_READ      4

/* What `diag_eth_probe` found. */
struct diag_eth {
    int state; /* one of DIAG_ETH_* */
    uint8_t bus;
    uint8_t device;
    uint8_t function;
    uint64_t mmio; /* the window's physical address, 0 for none */
    uint32_t txconfig;
    uint32_t xid;
};

/** Log one line formatted already. */
static void diag_say(const char *line)
{
    klog(KLOG_INFO, "diag", "%s", line);
}

/**
 * Read the identity of the first RTL8168 on the bus into `*out`, without
 * configuring anything: its location, the first memory BAR by index (the
 * reference driver's choice), and its TxConfig and XID, or the state that
 * says why they were not read.
 *
 * If the firmware left memory decoding off, the window cannot be read and the
 * state says so: turning decoding on would hand the drivers that follow a
 * device this report had configured. The upper half of a 64-bit BAR is the
 * next slot and has address bits, not a window of its own, so it is stepped
 * over.
 */
static void diag_eth_probe(struct diag_eth *out)
{
    uintptr_t mapped;
    pci_device_t dev;
    uint16_t command;
    int i;

    out->state = DIAG_ETH_ABSENT;
    out->bus = 0;
    out->device = 0;
    out->function = 0;
    out->mmio = 0;
    out->txconfig = 0;
    out->xid = 0;

    if (!pci_find_device_by_id(DIAG_REALTEK_VENDOR, DIAG_RTL8168_DEVICE, &dev)) {
        return;
    }
    out->bus = dev.bus;
    out->device = dev.device;
    out->function = dev.function;

    command = pci_read_config16(dev.bus, dev.device, dev.function, PCI_COMMAND);
    if (!(command & PCI_CMD_MEMORY_SPACE)) {
        out->state = DIAG_ETH_NO_DECODE;
        return;
    }

    for (i = 0; i < 6; i++) {
        if (!pci_bar_is_memory(dev.bar[i])) {
            continue;
        }
        out->mmio = pci_bar_address64(&dev, i);
        if (out->mmio != 0) {
            break;
        }
        if (pci_bar_is_64bit(dev.bar[i])) {
            i++;
        }
    }
    if (out->mmio == 0 || (uint64_t)(uintptr_t)out->mmio != out->mmio) {
        out->state = DIAG_ETH_NO_WINDOW;
        return;
    }

    mapped = map_mmio((uintptr_t)out->mmio, DIAG_MMIO_WINDOW);
    if (mapped == 0) {
        out->state = DIAG_ETH_NO_MAP;
        return;
    }

    out->txconfig = *(volatile uint32_t *)(mapped + DIAG_TXCONFIG);
    out->xid = (out->txconfig >> DIAG_XID_SHIFT) & DIAG_XID_MASK;
    out->state = DIAG_ETH_READ;
}

/** Log what the peek found, or why it found nothing. */
static void diag_eth_report(const struct diag_eth *e)
{
    if (e->state == DIAG_ETH_ABSENT) {
        diag_say("  ethernet: no Realtek 10ec:8168 on the bus");
        return;
    }

    klog(KLOG_INFO, "diag", "ethernet: 10ec:8168 at %d:%d.%d", (uint32_t)e->bus,
         (uint32_t)e->device, (uint32_t)e->function);

    if (e->state == DIAG_ETH_NO_DECODE) {
        diag_say("  ethernet: memory decoding is off; the chip id is not read");
        return;
    }
    if (e->state == DIAG_ETH_NO_WINDOW) {
        diag_say("  ethernet: no memory window; the chip id is not read");
        return;
    }
    if (e->state == DIAG_ETH_NO_MAP) {
        diag_say("  ethernet: the window did not map; the chip id is not read");
        return;
    }

    klog(KLOG_INFO, "diag", "ethernet: mmio 0x%lX txconfig 0x%X xid 0x%X", (unsigned long)e->mmio,
         e->txconfig, e->xid);
    if ((e->xid & DIAG_XID_FAMILY_MASK) == DIAG_XID_RTL8168GU) {
        diag_say("  ethernet: RTL8168GU, the chip this port targets");
    } else {
        diag_say("  ethernet: not RTL8168GU; the port refuses this chip");
    }
}

/**
 * Log the facts the rest of the boot printed, gathered onto one screen: the
 * board's power profile, the first network device and its station address,
 * the disk policy, the entropy source and the nodes of the KRFFS image.
 *
 * The screen holds about twenty-five lines and the boot prints more, so
 * what the drivers reported has scrolled off by the time anything can be
 * read. Each of these is one line somewhere in that scroll, and each is a
 * fact a first bring-up is there to collect, so they are asked for again
 * from the modules that keep them.
 */
static void diag_summary(void)
{
    diag_say("======= What this boot found =======");

    klog(KLOG_INFO, "diag", "board: %s", board_name());

    klog(KLOG_INFO, "diag", "network: %s, station %X%X:%X%X:%X%X:%X%X:%X%X:%X%X",
         netdev_name(netdev_first()), (unsigned)(net_mac[0] >> 4), (unsigned)(net_mac[0] & 0xF),
         (unsigned)(net_mac[1] >> 4), (unsigned)(net_mac[1] & 0xF), (unsigned)(net_mac[2] >> 4),
         (unsigned)(net_mac[2] & 0xF), (unsigned)(net_mac[3] >> 4), (unsigned)(net_mac[3] & 0xF),
         (unsigned)(net_mac[4] >> 4), (unsigned)(net_mac[4] & 0xF), (unsigned)(net_mac[5] >> 4),
         (unsigned)(net_mac[5] & 0xF));

    klog(KLOG_INFO, "diag", "disk policy: %s",
         diskcmd_policy() == DISKCMD_READ_WRITE ? "read-write" : "read-only");

    klog(KLOG_INFO, "diag", "entropy: %s", krandom_is_trusted() ? "trusted" : "not trusted");

    klog(KLOG_INFO, "diag", "file system: %d files", (uint32_t)krffs_count_files());

    diag_say("===================================");
}

/**
 * The first report goes out before any driver runs, which is where its
 * numbers are worth having and also where a machine with no serial port
 * cannot read them: the boot log scrolls past in well under a second, and
 * the login prompt is all that is left on the screen.
 *
 * The wait is a polled read of the 8042 rather than anything driven by
 * interrupts, so that it holds whatever else is or is not working. A byte
 * on the serial line ends it too, since that is how the project's harnesses
 * type at a machine. Whatever either had buffered is drained first, so a
 * key pressed earlier in the boot does not end the wait before the screen
 * is read. A machine without an 8042, whose port 0x64 reads 0xFF as an
 * empty bus does, has a byte waiting after every read; one that still has
 * one after the sixteen bytes of Linux's flush (`i8042_flush`) has no
 * controller, and the serial line alone ends the wait.
 */
void boot_diag_hold(void)
{
    int kbc = 1, n;

    boot_diag_report();
    diag_summary();
    klog(KLOG_INFO, "diag", "holding for a keypress");

    for (n = 0; (inb(0x64) & 0x01u) != 0; n++) {
        if (n == 16) {
            kbc = 0;
            klog(KLOG_INFO, "diag", "no 8042: the serial line releases the hold");
            break;
        }
        (void)inb(0x60);
    }
    while (serial_data_available()) {
        (void)serial_try_getc();
    }

    for (;;) {
        if (kbc && (inb(0x64) & 0x01u) != 0) {
            (void)inb(0x60);
            return;
        }
        if (serial_data_available()) {
            (void)serial_try_getc();
            return;
        }
    }
}

/** The Ethernet peek runs before any figure is logged, though it is
 * reported last: mapping the controller's window can take a page table from
 * the boot allocator, and a memory figure logged before that would not be
 * the memory the machine has left. */
void boot_diag_report(void)
{
    struct diag_eth eth;
    uint32_t i;

    diag_eth_probe(&eth);

    diag_say("");
    diag_say("======= Boot diagnostics (rach2.diag) =======");

    klog(KLOG_INFO, "diag", "kernel ends 0x%lx, reservation break 0x%lx",
         (unsigned long)(uintptr_t)&_end, (unsigned long)mem_reserved_ptr);
    klog(KLOG_INFO, "diag", "direct map ends 0x%lx, %lu frames free after all reservations",
         (unsigned long)HARD_MEMORY_LIMIT, (unsigned long)frame_free_count());

    if (module_count == 0) {
        diag_say("  modules: none (the kernel booted alone)");
    }
    for (i = 0; i < module_count; i++) {
        module_info_t *mod = get_module(i);

        if (!mod) {
            continue;
        }
        klog(KLOG_INFO, "diag", "module %d: 0x%X to 0x%X, %d bytes", i, mod->phys,
             mod->phys + mod->size, mod->size);
    }

    diag_eth_report(&eth);
    diag_say("============================================");
}
