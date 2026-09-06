/*
 * The legacy ATA driver: the two channels of an IDE controller, each with
 * a master and a slave, driven by polled PIO.
 *
 * A channel is a pair of port ranges, the command block and the control
 * block, at the legacy addresses or where a controller in native mode says
 * in its PCI base registers. A drive is selected by one bit of the drive
 * register, and every command is issued to the drive selected. Each drive
 * that answers IDENTIFY with the disk signature is published as a block
 * device and registered with the disk registry; the first found is
 * `block0`. Transfers are `READ SECTORS` and `WRITE SECTORS`, in LBA28 when
 * the sectors lie below 2^28 and in their 48-bit forms above, on a drive
 * that has them; a write is followed by a cache flush. Everything polls:
 * the channel's interrupt is masked, and the waits count iterations, since
 * the page-cache window faults into this driver with interrupts off and no
 * timer to bound by. It is the emulator's disk and a transfer is short.
 */

#include "../klog.h"
#include "block.h"
#include "ata.h"
#include "diskcmd.h"
#include "disk.h"

#include "../kio.h"
#include "../pci.h"
#include "../serial.h"

#include "../lib/kerrno.h"
#include "../lib/kint.h"

/* Command-block registers, as offsets from the channel's command base. */
#define ATA_REG_DATA     0  /* 16-bit data port */
#define ATA_REG_SECCOUNT 2  /* sector count (0 means 256) */
#define ATA_REG_LBA_LOW  3  /* LBA bits 0-7 */
#define ATA_REG_LBA_MID  4  /* LBA bits 8-15 */
#define ATA_REG_LBA_HIGH 5  /* LBA bits 16-23 */
#define ATA_REG_DRIVE    6  /* drive select + LBA bits 24-27 */
#define ATA_REG_STATUS   7  /* status (read) / command (write) */

/* Status register bits. */
#define ATA_SR_BSY 0x80  /* busy */
#define ATA_SR_DF  0x20  /* device fault */
#define ATA_SR_DRQ 0x08  /* data request ready */
#define ATA_SR_ERR 0x01  /* error */

/* The commands this driver issues. */
#define ATA_CMD_READ       0x20
#define ATA_CMD_READ_EXT   0x24
#define ATA_CMD_WRITE      0x30
#define ATA_CMD_WRITE_EXT  0x34
#define ATA_CMD_FLUSH      0xE7
#define ATA_CMD_FLUSH_EXT  0xEA
#define ATA_CMD_IDENTIFY   0xEC

/* The drive register: bit 4 selects the slave, bit 6 LBA addressing, and
   in LBA28 the low four bits are LBA bits 24-27. */
#define ATA_DRIVE_SELECT 0xA0
#define ATA_DRIVE_LBA    0x40
#define ATA_DRIVE_SLAVE  0x10

/* One command moves at most 256 sectors: the legacy count register is
   8-bit, with 0 meaning 256, and the 48-bit form is kept to the same. */
#define ATA_MAX_SECTORS_PER_CMD 256
#define ATA_WORDS_PER_SECTOR    (BLOCK_SECTOR_SIZE / 2)

/* The largest LBA the 28-bit commands address, plus one. */
#define ATA_LBA28_SECTORS 0x10000000ull

/* A bound on every status wait, so a drive that never answers cannot leave
   the machine hanging. With interrupts off there is no timer to bound by,
   so the watchdog counts iterations instead of time. */
#define ATA_SPIN_LIMIT 10000000

/* The IDE controller class, and the prog-if bits that say a channel is in
   native mode, with its ports in the base address registers. */
#define PCI_CLASS_STORAGE 0x01
#define PCI_SUBCLASS_IDE  0x01
#define ATA_PROGIF_PRIMARY_NATIVE   0x01
#define ATA_PROGIF_SECONDARY_NATIVE 0x04

struct ata_channel {
    uint16_t io;   /* the command block */
    uint16_t ctrl; /* the control block: alternate status and device control */
    const char *name;
};

struct ata_drive {
    struct ata_channel *channel;
    int slave;
    int lba48;
    uint64_t sectors;
    int present;
    block_device_t dev;
};

static struct ata_channel channels[2] = {{0x1F0, 0x3F6, "primary"}, {0x170, 0x376, "secondary"}};
static struct ata_drive drives[4];

static int ata_read(block_device_t *device, uint64_t lba, uint32_t count, void *buffer);
static int ata_write(block_device_t *device, uint64_t lba, uint32_t count, const void *buffer);

/** Issue `command` to the selected drive of `ch` unless the policy refuses it. */
static int ata_command(const struct ata_channel *ch, uint8_t command)
{
    if (!diskcmd_allowed(command)) {
        klog(KLOG_WARN, "ata", "refused: command 0x%X: %s", (unsigned)command,
             diskcmd_policy_banner);
        return 0;
    }
    block_count_command();
    if (command == ATA_CMD_FLUSH || command == ATA_CMD_FLUSH_EXT) {
        block_count_flush();
    }
    outb(ch->io + ATA_REG_STATUS, command);
    return 1;
}

/** The 400 ns a drive needs after a select or a command before its status
 * means anything: four reads of the alternate status register. */
static void ata_settle(const struct ata_channel *ch)
{
    inb(ch->ctrl);
    inb(ch->ctrl);
    inb(ch->ctrl);
    inb(ch->ctrl);
}

static int ata_wait_not_busy(const struct ata_channel *ch)
{
    int spin = ATA_SPIN_LIMIT;

    while (spin-- > 0) {
        if ((inb(ch->io + ATA_REG_STATUS) & ATA_SR_BSY) == 0) {
            return 0;
        }
    }
    return -EIO;
}

/** Wait until the drive is idle with no data pending; an error is `-EIO`. */
static int ata_wait_ready(const struct ata_channel *ch)
{
    int spin = ATA_SPIN_LIMIT;

    while (spin-- > 0) {
        uint8_t status = inb(ch->io + ATA_REG_STATUS);

        if (status & ATA_SR_BSY) {
            continue;
        }
        if (status & (ATA_SR_ERR | ATA_SR_DF)) {
            return -EIO;
        }
        if (status & ATA_SR_DRQ) {
            continue;
        }
        return 0;
    }
    return -EIO;
}

/** Wait until the drive has a sector to move; an error is `-EIO`. */
static int ata_wait_data(const struct ata_channel *ch)
{
    int spin = ATA_SPIN_LIMIT;

    while (spin-- > 0) {
        uint8_t status = inb(ch->io + ATA_REG_STATUS);

        if (status & ATA_SR_BSY) {
            continue;
        }
        if (status & (ATA_SR_ERR | ATA_SR_DF)) {
            return -EIO;
        }
        if (status & ATA_SR_DRQ) {
            return 0;
        }
    }
    return -EIO;
}

/**
 * Select `d` and program `count` sectors from `lba`, in the 28-bit form
 * when the range lies below 2^28 and the 48-bit form otherwise. Returns
 * the read or write command to issue for that form, 0 or 1 for `write`.
 */
static uint8_t ata_program(const struct ata_drive *d, uint64_t lba, uint32_t count, int write)
{
    const struct ata_channel *ch = d->channel;
    uint8_t slave = d->slave ? ATA_DRIVE_SLAVE : 0;

    if (lba + count <= ATA_LBA28_SECTORS) {
        outb(ch->io + ATA_REG_DRIVE,
             (uint8_t)(ATA_DRIVE_SELECT | ATA_DRIVE_LBA | slave | ((lba >> 24) & 0x0F)));
        outb(ch->io + ATA_REG_SECCOUNT, count == ATA_MAX_SECTORS_PER_CMD ? 0 : (uint8_t)count);
        outb(ch->io + ATA_REG_LBA_LOW, (uint8_t)lba);
        outb(ch->io + ATA_REG_LBA_MID, (uint8_t)(lba >> 8));
        outb(ch->io + ATA_REG_LBA_HIGH, (uint8_t)(lba >> 16));
        return write ? ATA_CMD_WRITE : ATA_CMD_READ;
    }
    /* The 48-bit form: each register is written twice, the high byte first,
       and the drive register has no address bits. */
    outb(ch->io + ATA_REG_DRIVE, (uint8_t)(ATA_DRIVE_SELECT | ATA_DRIVE_LBA | slave));
    outb(ch->io + ATA_REG_SECCOUNT, (uint8_t)(count >> 8));
    outb(ch->io + ATA_REG_LBA_LOW, (uint8_t)(lba >> 24));
    outb(ch->io + ATA_REG_LBA_MID, (uint8_t)(lba >> 32));
    outb(ch->io + ATA_REG_LBA_HIGH, (uint8_t)(lba >> 40));
    outb(ch->io + ATA_REG_SECCOUNT, (uint8_t)count);
    outb(ch->io + ATA_REG_LBA_LOW, (uint8_t)lba);
    outb(ch->io + ATA_REG_LBA_MID, (uint8_t)(lba >> 8));
    outb(ch->io + ATA_REG_LBA_HIGH, (uint8_t)(lba >> 16));
    return write ? ATA_CMD_WRITE_EXT : ATA_CMD_READ_EXT;
}

static int ata_read(block_device_t *device, uint64_t lba, uint32_t count, void *buffer)
{
    struct ata_drive *d = (struct ata_drive *)device->drvdata;
    const struct ata_channel *ch = d->channel;
    uint8_t *out = (uint8_t *)buffer;

    if (count == 0 || lba > device->sector_count || count > device->sector_count - lba) {
        return -EIO;
    }
    while (count > 0) {
        uint32_t chunk = (count > ATA_MAX_SECTORS_PER_CMD) ? ATA_MAX_SECTORS_PER_CMD : count;
        uint32_t i;

        if (ata_wait_not_busy(ch) != 0) {
            return -EIO;
        }
        if (!ata_command(ch, ata_program(d, lba, chunk, 0))) {
            return -EIO;
        }
        ata_settle(ch);
        for (i = 0; i < chunk; ++i) {
            if (ata_wait_data(ch) != 0) {
                return -EIO;
            }
            insw(ch->io + ATA_REG_DATA, out, ATA_WORDS_PER_SECTOR);
            out += BLOCK_SECTOR_SIZE;
        }
        lba += chunk;
        count -= chunk;
    }
    return 0;
}

/**
 * Write whole sectors, then flush the drive's cache, so a returned write is
 * on the medium; the flush is the 48-bit form on a drive that has it.
 */
static int ata_write(block_device_t *device, uint64_t lba, uint32_t count, const void *buffer)
{
    struct ata_drive *d = (struct ata_drive *)device->drvdata;
    const struct ata_channel *ch = d->channel;
    const uint8_t *in = (const uint8_t *)buffer;

    if (count == 0 || lba > device->sector_count || count > device->sector_count - lba) {
        return -EIO;
    }
    while (count > 0) {
        uint32_t chunk = (count > ATA_MAX_SECTORS_PER_CMD) ? ATA_MAX_SECTORS_PER_CMD : count;
        uint32_t i;

        if (ata_wait_not_busy(ch) != 0) {
            return -EIO;
        }
        if (!ata_command(ch, ata_program(d, lba, chunk, 1))) {
            return -EPERM;
        }
        ata_settle(ch);
        for (i = 0; i < chunk; ++i) {
            if (ata_wait_data(ch) != 0) {
                return -EIO;
            }
            outsw(ch->io + ATA_REG_DATA, in, ATA_WORDS_PER_SECTOR);
            in += BLOCK_SECTOR_SIZE;
        }
        if (ata_wait_ready(ch) != 0) {
            return -EIO;
        }
        if (!ata_command(ch, d->lba48 ? ATA_CMD_FLUSH_EXT : ATA_CMD_FLUSH)) {
            return -EPERM;
        }
        ata_settle(ch);
        if (ata_wait_ready(ch) != 0) {
            return -EIO;
        }
        lba += chunk;
        count -= chunk;
    }
    return 0;
}

/**
 * IDENTIFY the drive `d` and fill its record. Returns 0 for a disk, -1
 * for an absent drive, a packet device, or a drive that did not answer.
 *
 * A channel with no drive leaves the bus floating, which reads as 0 on
 * emulators and 0xFF on most real hardware; 0xFF has BSY set, so without
 * that check an absent drive would spin out the whole watchdog. A packet
 * device answers IDENTIFY with the ATAPI signature in the LBA registers
 * and is refused. The 48-bit sector count is words 100-103 when word 83
 * says the drive has the 48-bit commands, else the 28-bit count in words
 * 60-61; a 28-bit drive is capped at what its commands address.
 */
static int ata_identify(struct ata_drive *d, uint16_t *identify)
{
    const struct ata_channel *ch = d->channel;
    uint8_t status;

    outb(ch->io + ATA_REG_DRIVE, (uint8_t)(ATA_DRIVE_SELECT | (d->slave ? ATA_DRIVE_SLAVE : 0)));
    ata_settle(ch);
    outb(ch->io + ATA_REG_SECCOUNT, 0);
    outb(ch->io + ATA_REG_LBA_LOW, 0);
    outb(ch->io + ATA_REG_LBA_MID, 0);
    outb(ch->io + ATA_REG_LBA_HIGH, 0);
    if (!ata_command(ch, ATA_CMD_IDENTIFY)) {
        return -1;
    }
    ata_settle(ch);
    status = inb(ch->io + ATA_REG_STATUS);
    if (status == 0 || status == 0xFF) {
        return -1;
    }
    if (ata_wait_not_busy(ch) != 0) {
        return -1;
    }
    if (inb(ch->io + ATA_REG_LBA_MID) != 0 || inb(ch->io + ATA_REG_LBA_HIGH) != 0) {
        return -1;
    }
    if (ata_wait_data(ch) != 0) {
        return -1;
    }
    insw(ch->io + ATA_REG_DATA, identify, ATA_WORDS_PER_SECTOR);
    d->lba48 = (identify[83] & 0x0400u) != 0;
    if (d->lba48) {
        d->sectors = (uint64_t)identify[100] | ((uint64_t)identify[101] << 16) |
                     ((uint64_t)identify[102] << 32) | ((uint64_t)identify[103] << 48);
    }
    if (!d->lba48 || d->sectors == 0) {
        d->lba48 = 0;
        d->sectors = (uint64_t)identify[60] | ((uint64_t)identify[61] << 16);
        if (d->sectors > ATA_LBA28_SECTORS) {
            d->sectors = ATA_LBA28_SECTORS;
        }
    }
    return d->sectors == 0 ? -1 : 0;
}

/**
 * Take a channel's ports from the controller's base registers when its
 * prog-if says the channel is in native mode; the legacy ports otherwise.
 */
static void ata_native_ports(void)
{
    pci_device_t ide;
    uint8_t progif;

    if (!pci_find_device(PCI_CLASS_STORAGE, PCI_SUBCLASS_IDE, &ide)) {
        return;
    }
    progif = pci_read_config8(ide.bus, ide.device, ide.function, 0x09);
    if (progif & ATA_PROGIF_PRIMARY_NATIVE) {
        channels[0].io = (uint16_t)(pci_read_config32(ide.bus, ide.device, ide.function, 0x10) & ~3u);
        channels[0].ctrl =
            (uint16_t)((pci_read_config32(ide.bus, ide.device, ide.function, 0x14) & ~3u) + 2);
    }
    if (progif & ATA_PROGIF_SECONDARY_NATIVE) {
        channels[1].io = (uint16_t)(pci_read_config32(ide.bus, ide.device, ide.function, 0x18) & ~3u);
        channels[1].ctrl =
            (uint16_t)((pci_read_config32(ide.bus, ide.device, ide.function, 0x1C) & ~3u) + 2);
    }
}

/** Whether two IDENTIFY answers name one drive: the same serial and model. */
static int same_drive(const uint16_t *a, const uint16_t *b)
{
    int w;

    for (w = 10; w < 47; w++) {
        if (a[w] != b[w]) {
            return 0;
        }
    }
    return 1;
}

int ata_init(void)
{
    static uint16_t identify[4][ATA_WORDS_PER_SECTOR];
    int c, s, found = 0;

    ata_native_ports();
    for (c = 0; c < 2; c++) {
        struct ata_channel *ch = &channels[c];

        /* nIEN in the device control register: the channel raises no
           interrupt, since this driver only polls. */
        outb(ch->ctrl, 0x02);
        for (s = 0; s < 2; s++) {
            struct ata_drive *d = &drives[c * 2 + s];

            d->channel = ch;
            d->slave = s;
            d->present = 0;
            if (ata_identify(d, identify[c * 2 + s]) != 0) {
                continue;
            }
            /* A channel with a master alone answers for its slave with the
             * master's own registers on some controllers; a slave that
             * IDENTIFYs as its master is that echo, not a second disk. */
            if (s == 1 && drives[c * 2].present && same_drive(identify[c * 2], identify[c * 2 + 1])) {
                continue;
            }
            d->present = 1;
            d->dev.sector_size = BLOCK_SECTOR_SIZE;
            d->dev.sector_count = d->sectors;
            d->dev.flags = BLOCK_FAULT_SAFE;
            d->dev.read = ata_read;
            d->dev.write = ata_write;
            d->dev.drvdata = d;
            if (!block0) {
                block0 = &d->dev;
            }
            disk_register(&d->dev, "ata", c * 2 + s, identify[c * 2 + s]);
            klog(KLOG_INFO, "ata", "%s %s: %d sectors, %d MiB%s", ch->name,
                 s ? "slave" : "master", (uint32_t)d->sectors, (uint32_t)(d->sectors / 2048),
                 d->lba48 ? ", LBA48" : "");
            found++;
        }
    }
    return found > 0 ? 0 : -1;
}
