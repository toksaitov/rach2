/*
 * diag.c: the `rach2.diag` boot report.
 *
 * The Lenovo this was written for has no serial port, so a first bring-up sees
 * only what reaches the screen. These are the numbers that decide whether a
 * boot is worth continuing: the layout the bootloader chose (which the kernel
 * has just judged), what memory is left after the reservations, and the
 * identity of the Ethernet controller, read from the chip, not assumed
 * from its PCI device id, because Realtek ships one id across a family whose
 * members need different bring-up sequences.
 *
 * The Ethernet peek runs before any figure is printed, even though it is
 * reported last: mapping the controller's window can carve a page table off
 * the boot allocator, and a memory figure printed before that is not the
 * memory the machine has left.
 *
 * Every line is a record, so it reaches the screen and the serial port and
 * the machine's own log from one call. Between the first two they are the
 * only answer two different machines give: the VGA text buffer is what a
 * camera or a capture card reads on a laptop with no serial header, and the
 * serial line is what a harness reads under QEMU. Before the kernel log this
 * file named both printers on every line, which is half of what the log was
 * written for.
 */

#include "klog.h"
#include "diag.h"
#include "kio.h"
#include "serial.h"
#include "module.h"
#include "pci.h"
#include "board.h"
#include "block/diskcmd.h"
#include "fs/krffs.h"
#include "lib/krandom.h"
#include "network/net.h"
#include "network/driver/netdev.h"
#include "mm/heap.h"
#include "mm/paging.h"

/* The Realtek gigabit family. The device id alone spans RTL8168B through H, so
 * the identity that matters is the chip's own XID, below. */
#define DIAG_REALTEK_VENDOR 0x10EC
#define DIAG_RTL8168_DEVICE 0x8168

/* TxConfig, whose top bits carry the hardware revision Linux calls the XID.
 * The mask and shift are the reference driver's (`(TxConfig >> 20) & 0xfcf`),
 * and RTL8168GU answers `(xid & 0x7cf) == 0x509`. */
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

struct diag_eth {
    int state;
    uint8_t bus;
    uint8_t device;
    uint8_t function;
    uint32_t mmio;
    uint32_t txconfig;
    uint32_t xid;
};

/** Report one already-formatted line. */
static void diag_say(const char *line)
{
    klog(KLOG_INFO, "diag", "%s", line);
}

/**
 * Read the Ethernet controller's identity, without configuring anything.
 *
 * If the firmware left memory decoding off, the window cannot be read and the
 * state says so: turning decoding on would hand the drivers that follow a
 * device this report had configured, which is the one thing a diagnostic must
 * not do.
 */
static void diag_eth_probe(struct diag_eth *out)
{
    uintptr_t mapped;
    pci_device_t dev;
    uint16_t command;
    uint32_t bar = 0;
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

    /* The first memory BAR by index, which is what the reference driver takes.
     * A 64-bit BAR's upper half occupies the next slot and has address bits,
     * not a window of its own, so it is stepped over, not considered. */
    for (i = 0; i < 6; i++) {
        if (!pci_bar_is_memory(dev.bar[i])) {
            continue;
        }
        if (pci_bar_address(dev.bar[i]) != 0) {
            bar = dev.bar[i];
            break;
        }
        if (pci_bar_is_64bit(dev.bar[i])) {
            i++;
        }
    }
    if (bar == 0) {
        out->state = DIAG_ETH_NO_WINDOW;
        return;
    }

    out->mmio = pci_bar_address(bar);
    mapped = map_mmio(out->mmio, DIAG_MMIO_WINDOW);
    if (mapped == 0) {
        out->state = DIAG_ETH_NO_MAP;
        return;
    }

    out->txconfig = *(volatile uint32_t *)(mapped + DIAG_TXCONFIG);
    out->xid = (out->txconfig >> DIAG_XID_SHIFT) & DIAG_XID_MASK;
    out->state = DIAG_ETH_READ;
}

/** Print what the peek found, or why it found nothing. */
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

    klog(KLOG_INFO, "diag", "ethernet: mmio 0x%X txconfig 0x%X xid 0x%X", e->mmio, e->txconfig,
         e->xid);
    if ((e->xid & DIAG_XID_FAMILY_MASK) == DIAG_XID_RTL8168GU) {
        diag_say("  ethernet: RTL8168GU, the chip this port targets");
    } else {
        diag_say("  ethernet: not RTL8168GU; the port refuses this chip");
    }
}

/**
 * The facts the rest of the boot printed, gathered onto one screen.
 *
 * Re-printing the early report was not enough and the reason is the screen
 * itself: it holds about twenty-five lines and the boot prints half as many
 * again, so everything the drivers reported has scrolled off by the time
 * anything can be read. The chipset profile, the station address, the disk
 * policy and the entropy source are each one line somewhere in that scroll,
 * and each is a fact a first bring-up is there to collect.
 *
 * So they are asked for again here, from the modules that hold them, rather
 * than being recovered from a screen that no longer has them.
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
 * Print the report again and hold the screen until a key is pressed.
 *
 * The first report goes out before any driver runs, which is where its numbers
 * are worth having and also where a machine with no serial port cannot read
 * them: the whole boot log scrolls past in well under a second, and the only
 * thing left on the screen is the login prompt. Photographing it needs a video
 * camera, which is what the first laptop session actually resorted to.
 *
 * So under `rach2.diag` the report is printed a second time at the end of the
 * boot, where the keyboard is up and nothing further will scroll, and the
 * machine waits. The wait is a polled read of the 8042 instead of anything
 * interrupt-driven, so it holds whatever else is or is not working. A byte on
 * the serial line releases it too, since that is how this project's harnesses
 * type at a machine.
 */
void boot_diag_hold(void)
{
    boot_diag_report();
    diag_summary();
    klog(KLOG_INFO, "diag", "holding for a keypress");

    /* Drain whatever is already buffered, so a keystroke from earlier in the
     * boot does not consume the hold before anyone has read the screen. */
    while ((inb(0x64) & 0x01u) != 0) {
        (void)inb(0x60);
    }
    while (serial_data_available()) {
        (void)serial_try_getc();
    }

    /* Either input releases it: a key on the machine's own keyboard, which is
     * what the laptop has, or a byte on the serial line, which is how every
     * harness in this project drives a machine. */
    for (;;) {
        if ((inb(0x64) & 0x01u) != 0) {
            (void)inb(0x60);
            return;
        }
        if (serial_data_available()) {
            (void)serial_try_getc();
            return;
        }
    }
}

void boot_diag_report(void)
{
    struct diag_eth eth;
    uint32_t i;

    /* The peek first, printing second: mapping the window can carve a page
     * table, and the figures below are meant to be what is left after every
     * boot-time reservation, this report's own included. */
    diag_eth_probe(&eth);

    diag_say("");
    diag_say("======= Boot diagnostics (rach2.diag) =======");

    klog(KLOG_INFO, "diag", "kernel ends 0x%X, reservation break 0x%X", (uint32_t)(uintptr_t)&_end,
         (uint32_t)mem_reserved_ptr);
    klog(KLOG_INFO, "diag", "direct map ends 0x%X, %d frames free after all reservations",
         (uint32_t)HARD_MEMORY_LIMIT, frame_free_count());

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
