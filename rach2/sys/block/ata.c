#include "block.h"
#include "ata.h"
#include "diskcmd.h"
#include "diskinfo.h"

#include "../kio.h"
#include "../serial.h"

#include "../lib/kerrno.h"
#include "../lib/kint.h"

/* Primary ATA channel: the command-block and control-block port bases. */
#define ATA_IO_BASE   0x1F0
#define ATA_CTRL_BASE 0x3F6

/* Command-block registers, as offsets from `ATA_IO_BASE`. */
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

/* Commands. */
#define ATA_CMD_READ     0x20
#define ATA_CMD_WRITE    0x30
#define ATA_CMD_FLUSH    0xE7
#define ATA_CMD_IDENTIFY 0xEC

/* Drive-select byte for the master in LBA mode; the low nibble has LBA bits
   24-27. */
#define ATA_DRIVE_LBA_MASTER 0xE0
#define ATA_DRIVE_MASTER     0xA0

/* The transfer is one command per chunk of up to 256 sectors (the count
   register is 8-bit, with 0 meaning 256). */
#define ATA_MAX_SECTORS_PER_CMD 256
#define ATA_WORDS_PER_SECTOR    (BLOCK_SECTOR_SIZE / 2)

/* The LBA28 commands address sectors 0 to 2^28-1, so the most a disk can hold
   through this driver is 2^28 sectors (128 GiB). */
#define ATA_LBA28_MAX_SECTORS 0x10000000u

/* A bounded spin so a wedged or absent drive returns `-EIO` rather than
   hanging. With interrupts off there is no timer to bound by, so the watchdog
   counts iterations instead of time. */
#define ATA_SPIN_LIMIT 10000000

static int ata_read(
               block_device_t *device, uint64_t lba, uint32_t count,
               void *buffer
           );
static int ata_write(
               block_device_t *device, uint64_t lba, uint32_t count,
               const void *buffer
           );

static block_device_t ata_device;

/**
 * Issue one command to the drive, or refuse it. Returns 1 if it was issued.
 *
 * The one place a command leaves this driver, which is what makes the build's
 * disk policy enforceable: a kernel built read-only refuses here, whatever the
 * caller believed it was doing, and says so once so a refused boot has a
 * reason on the screen rather than an unexplained error code.
 */
static int ata_command(uint8_t command)
{
    if (!diskcmd_allowed(command)) {
        serial_printf("ata: refused command 0x%X: %s\n", (unsigned)command,
                      diskcmd_policy_banner);
        return 0;
    }
    block_count_command();
    if (command == ATA_CMD_FLUSH) {
        block_count_flush();
    }
    outb(ATA_IO_BASE + ATA_REG_STATUS, command);
    return 1;
}

/**
 * Wastes the roughly 400 ns the drive needs to settle after a command before
 * its status bits are valid, by reading the alternate-status port four times.
 */
static void ata_settle(void)
{
    inb(ATA_CTRL_BASE);
    inb(ATA_CTRL_BASE);
    inb(ATA_CTRL_BASE);
    inb(ATA_CTRL_BASE);
}

/**
 * Spins until the drive clears BSY. Returns 0 when it does, `-EIO` if the
 * bounded watchdog expires first.
 */
static int ata_wait_not_busy(void)
{
    int spin = ATA_SPIN_LIMIT;

    while (spin-- > 0) {
        if ((inb(ATA_IO_BASE + ATA_REG_STATUS) & ATA_SR_BSY) == 0) {
            return 0;
        }
    }

    return -EIO;
}

/**
 * Spins until the drive finishes the current command (BSY and DRQ both
 * clear), then reports whether it errored. Returns 0 on a clean completion,
 * `-EIO` on an ERR/DF status or watchdog expiry. Waiting for DRQ to clear as
 * well as BSY matters after a PIO data-out write: the drive can briefly drop
 * BSY with DRQ still set, and treating that as done would issue the next
 * command mid transfer. The status bits other than BSY are read only once
 * BSY is clear.
 */
static int ata_wait_ready(void)
{
    int spin = ATA_SPIN_LIMIT;

    while (spin-- > 0) {
        uint8_t status = inb(ATA_IO_BASE + ATA_REG_STATUS);

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

/**
 * Spins until the drive is ready to transfer a sector (BSY clear and DRQ set).
 * Returns 0 then, or `-EIO` on an ERR/DF status or watchdog expiry. The
 * BSY/DRQ handshake is per sector, so this is polled before each sector, and
 * the error and DRQ bits are read only after BSY clears, when they are valid.
 */
static int ata_wait_data(void)
{
    int spin = ATA_SPIN_LIMIT;

    while (spin-- > 0) {
        uint8_t status = inb(ATA_IO_BASE + ATA_REG_STATUS);

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
 * Programs the drive-select, sector-count, and LBA registers for an LBA28
 * transfer of `count` sectors (1 to 256) starting at `lba`.
 */
static void ata_program_lba(uint64_t lba, uint32_t count)
{
    uint8_t count_reg =
        (count == ATA_MAX_SECTORS_PER_CMD) ? 0 : (uint8_t) count;

    outb(ATA_IO_BASE + ATA_REG_DRIVE,
         (uint8_t) (ATA_DRIVE_LBA_MASTER | ((lba >> 24) & 0x0F)));
    outb(ATA_IO_BASE + ATA_REG_SECCOUNT, count_reg);
    outb(ATA_IO_BASE + ATA_REG_LBA_LOW,  (uint8_t) (lba & 0xFF));
    outb(ATA_IO_BASE + ATA_REG_LBA_MID,  (uint8_t) ((lba >> 8) & 0xFF));
    outb(ATA_IO_BASE + ATA_REG_LBA_HIGH, (uint8_t) ((lba >> 16) & 0xFF));
}

/**
 * Reads `count` sectors starting at `lba` into `buffer` with polled LBA28 PIO.
 *
 * Issues READ SECTORS for up to 256 sectors per command and copies each sector
 * only after its own BSY/DRQ handshake, so a wedged or short transfer returns
 * `-EIO` rather than corrupting `buffer`. Returns 0 on success.
 */
static int ata_read(
               block_device_t *device, uint64_t lba, uint32_t count,
               void *buffer
           )
{
    uint8_t *out = (uint8_t *) buffer;

    if (count == 0 || lba > device->sector_count ||
        count > device->sector_count - lba) {
        return -EIO;
    }

    while (count > 0) {
        uint32_t chunk =
            (count > ATA_MAX_SECTORS_PER_CMD) ? ATA_MAX_SECTORS_PER_CMD : count;
        uint32_t i;

        if (ata_wait_not_busy() != 0) {
            return -EIO;
        }
        ata_program_lba(lba, chunk);
        if (!ata_command(ATA_CMD_READ)) {
            return -EIO;
        }
        ata_settle();

        for (i = 0; i < chunk; ++i) {
            if (ata_wait_data() != 0) {
                return -EIO;
            }
            insw(ATA_IO_BASE + ATA_REG_DATA, out, ATA_WORDS_PER_SECTOR);
            out += BLOCK_SECTOR_SIZE;
        }

        lba += chunk;
        count -= chunk;
    }

    return 0;
}

/**
 * Writes `count` sectors from `buffer` to `lba` with polled LBA28 PIO, flushing
 * the drive's write cache after each command.
 *
 * Mirrors `ata_read` per chunk, then issues FLUSH CACHE so the device reports
 * its cached writes complete before a later fsync may treat them as durable.
 * The post-write and post-flush waits check ERR/DF, so a media or cache error
 * surfaces as `-EIO` rather than a false success. Returns 0 on success.
 */
static int ata_write(
               block_device_t *device, uint64_t lba, uint32_t count,
               const void *buffer
           )
{
    const uint8_t *in = (const uint8_t *) buffer;

    if (count == 0 || lba > device->sector_count ||
        count > device->sector_count - lba) {
        return -EIO;
    }

    while (count > 0) {
        uint32_t chunk =
            (count > ATA_MAX_SECTORS_PER_CMD) ? ATA_MAX_SECTORS_PER_CMD : count;
        uint32_t i;

        if (ata_wait_not_busy() != 0) {
            return -EIO;
        }
        ata_program_lba(lba, chunk);
        if (!ata_command(ATA_CMD_WRITE)) {
            return -EPERM;
        }
        ata_settle();

        for (i = 0; i < chunk; ++i) {
            if (ata_wait_data() != 0) {
                return -EIO;
            }
            outsw(ATA_IO_BASE + ATA_REG_DATA, in, ATA_WORDS_PER_SECTOR);
            in += BLOCK_SECTOR_SIZE;
        }

        if (ata_wait_ready() != 0) {
            return -EIO;
        }
        if (!ata_command(ATA_CMD_FLUSH)) {
            return -EPERM;
        }
        ata_settle();
        if (ata_wait_ready() != 0) {
            return -EIO;
        }

        lba += chunk;
        count -= chunk;
    }

    return 0;
}

int ata_init(void)
{
    uint16_t identify[ATA_WORDS_PER_SECTOR];
    uint64_t sector_count;

    block0 = NULL;

    /* Set nIEN in the device-control register so the channel raises no IRQ;
       this driver only polls, so the IDE interrupt line stays masked. */
    outb(ATA_CTRL_BASE, 0x02);

    /* Select the master and issue IDENTIFY with a zeroed LBA and count. */
    outb(ATA_IO_BASE + ATA_REG_DRIVE, ATA_DRIVE_MASTER);
    outb(ATA_IO_BASE + ATA_REG_SECCOUNT, 0);
    outb(ATA_IO_BASE + ATA_REG_LBA_LOW, 0);
    outb(ATA_IO_BASE + ATA_REG_LBA_MID, 0);
    outb(ATA_IO_BASE + ATA_REG_LBA_HIGH, 0);
    if (!ata_command(ATA_CMD_IDENTIFY)) {
        return -1;
    }
    ata_settle();

    {
        /* No drive leaves the bus floating: 0 on emulators, 0xFF on most real
           hardware (pulled-up lines). 0xFF also has BSY set, so without this
           check an absent drive would spin out the whole watchdog below. */
        uint8_t status = inb(ATA_IO_BASE + ATA_REG_STATUS);
        if (status == 0 || status == 0xFF) {
            return -1;
        }
    }
    if (ata_wait_not_busy() != 0) {
        return -1;
    }

    /* A non-zero LBA mid/high after BSY clears is a packet-device signature:
       an ATAPI or SATA drive speaks a different command set, so refuse it. */
    if (inb(ATA_IO_BASE + ATA_REG_LBA_MID) != 0 ||
        inb(ATA_IO_BASE + ATA_REG_LBA_HIGH) != 0) {
        return -1;
    }
    if (ata_wait_data() != 0) {
        return -1;
    }

    insw(ATA_IO_BASE + ATA_REG_DATA, identify, ATA_WORDS_PER_SECTOR);

    /* The 28-bit user-addressable sector count, in words 60-61. This driver
       issues only LBA28 commands, so it advertises exactly what those can
       address (the field saturates at 2^28 on a larger disk). */
    sector_count = (uint64_t) identify[60] |
                   ((uint64_t) identify[61] << 16);

    /* Never advertise more than the LBA28 commands can reach, even if the field
       somehow reports a larger count, so a bounds-checked LBA always fits. */
    if (sector_count > ATA_LBA28_MAX_SECTORS) {
        sector_count = ATA_LBA28_MAX_SECTORS;
    }

    if (sector_count == 0) {
        return -1;
    }

    diskinfo_record_identify("ata", -1, identify, sector_count);

    ata_device.sector_size = BLOCK_SECTOR_SIZE;
    ata_device.sector_count = sector_count;
    ata_device.read = ata_read;
    ata_device.write = ata_write;
    ata_device.drvdata = NULL;
    block0 = &ata_device;

    serial_printf("ata: primary master: %d sectors, %d MiB\n",
                  (uint32_t) sector_count, (uint32_t) (sector_count / 2048));
    return 0;
}
