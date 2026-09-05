/*
 * diskinfo.c: the disk's own account of itself, kept and rendered as text.
 *
 * The formatting is deliberately dull: fixed keys, one per line, decimal where
 * a number will be pasted into a `dd skip=` and hexadecimal where every
 * reference prints hexadecimal. What reads this is `awk` and `grep` on the
 * machine, so the shape matters more than the prose.
 */

#include "../klog.h"
#include "diskinfo.h"
#include "block.h"
#include "diskcmd.h"
#include "../fs/bcache.h"
#include "../fs/devfs.h"
#include "../serial.h"

static struct diskinfo info;

/* Set by diskinfo_record_identify before a probe adopts the device, so the
   identity survives even when the probe itself is what published block0. */
static int identify_seen;

void diskinfo_ata_string(char *out, int chars, const volatile uint16_t *id, int first_word)
{
    int i, last = -1;

    for (i = 0; i < chars; i++) {
        uint16_t w = id[first_word + (i / 2)];
        uint8_t c = (i & 1) ? (uint8_t)(w & 0xFF) : (uint8_t)(w >> 8);

        if (c < 0x20 || c > 0x7E) {
            c = (uint8_t)'?';
        }
        out[i] = (char)c;
        if (c != ' ') {
            last = i;
        }
    }
    out[last + 1] = '\0';
}

int diskinfo_u64_dec(char *out, int cap, uint64_t v)
{
    char tmp[24];
    int n = 0, i;

    if (cap <= 0) {
        return 0;
    }
    if (v == 0) {
        tmp[n++] = '0';
    }
    while (v != 0) {
        /* Long division by ten over 32-bit halves: the kernel links no libgcc,
         * so a 64-bit divide would be a call to a routine that is not there. */
        uint32_t hi = (uint32_t)(v >> 32);
        uint32_t lo = (uint32_t)(v & 0xFFFFFFFFu);
        uint32_t qhi = hi / 10u;
        uint32_t rem = hi % 10u;
        uint32_t qlo;
        uint32_t part;

        /* Fold the remainder of the high half into the low half a nibble at a
         * time, so no intermediate exceeds 32 bits. */
        qlo = 0;
        for (i = 28; i >= 0; i -= 4) {
            part = (rem << 4) | ((lo >> i) & 0xFu);
            qlo = (qlo << 4) | (part / 10u);
            rem = part % 10u;
        }
        tmp[n++] = (char)('0' + rem);
        v = ((uint64_t)qhi << 32) | (uint64_t)qlo;
    }
    if (n + 1 > cap) {
        out[0] = '\0';
        return 0;
    }
    for (i = 0; i < n; i++) {
        out[i] = tmp[n - 1 - i];
    }
    out[n] = '\0';
    return n;
}

uint64_t diskinfo_window(uint64_t sectors)
{
    if (sectors > (uint64_t)DISKINFO_WINDOW_MAX_SECTORS) {
        return (uint64_t)DISKINFO_WINDOW_MAX_SECTORS;
    }
    return sectors;
}

/** Append `s` to `buf` at `*n`, stopping at `cap - 1`. */
static void put(char *buf, int *n, int cap, const char *s)
{
    while (*s != '\0' && *n < cap - 1) {
        buf[(*n)++] = *s++;
    }
}

/** Append `v` as decimal. */
static void put_dec(char *buf, int *n, int cap, uint64_t v)
{
    char tmp[24];

    if (diskinfo_u64_dec(tmp, (int)sizeof(tmp), v) > 0) {
        put(buf, n, cap, tmp);
    }
}

/** Append `v` as `width` hexadecimal digits, lowercase. */
static void put_hex(char *buf, int *n, int cap, uint32_t v, int width)
{
    static const char digits[] = "0123456789abcdef";
    int i;

    for (i = width - 1; i >= 0; i--) {
        if (*n < cap - 1) {
            buf[(*n)++] = digits[(v >> (i * 4)) & 0xFu];
        }
    }
}

int diskinfo_format(const struct diskinfo *d, char *buf, int cap)
{
    int n = 0;
    int i;

    if (cap <= 0) {
        return 0;
    }
    put(buf, &n, cap, "state ");
    if (d->state == DISKINFO_PRESENT) {
        put(buf, &n, cap, "present\n");
    } else if (d->state == DISKINFO_NO_DEVICE) {
        put(buf, &n, cap, "no-device\n");
    } else {
        put(buf, &n, cap, "not-asked\n");
    }
    put(buf, &n, cap, "driver ");
    put(buf, &n, cap, d->driver ? d->driver : "none");
    put(buf, &n, cap, "\npolicy ");
    put(buf, &n, cap, diskcmd_policy() == DISKCMD_READ_ONLY ? "read-only\n" : "read-write\n");
    /* The buffer cache under the mounted file systems: reads served from a
     * buffer, reads that filled one, and waits for a buffer another process
     * held. Printed whatever the disk's state, since a mount can be on any
     * device the cache reads. */
    /* The drivers' own totals, printed whatever this disk's state: a mount can
     * be on any device they serve, and a row that counts what a write owed
     * the medium has no disk of its own to ask. */
    put(buf, &n, cap, "disk-commands ");
    put_dec(buf, &n, cap, block_commands());
    put(buf, &n, cap, "\ndisk-flushes ");
    put_dec(buf, &n, cap, block_flushes());
    put(buf, &n, cap, "\ncache-hits ");
    put_dec(buf, &n, cap, bcache_stat_hits());
    put(buf, &n, cap, "\ncache-fills ");
    put_dec(buf, &n, cap, bcache_stat_fills());
    put(buf, &n, cap, "\ncache-waits ");
    put_dec(buf, &n, cap, bcache_stat_waits());
    put(buf, &n, cap, "\n");

    if (d->state != DISKINFO_PRESENT) {
        buf[n] = '\0';
        return n;
    }

    put(buf, &n, cap, "port ");
    if (d->port < 0) {
        put(buf, &n, cap, "none");
    } else {
        put_dec(buf, &n, cap, (uint64_t)d->port);
    }
    put(buf, &n, cap, "\nmounted ");
    put(buf, &n, cap, d->mounted ? "1\n" : "0\n");
    put(buf, &n, cap, "model ");
    put(buf, &n, cap, d->model);
    put(buf, &n, cap, "\nserial ");
    put(buf, &n, cap, d->serial);
    put(buf, &n, cap, "\nfirmware ");
    put(buf, &n, cap, d->firmware);
    put(buf, &n, cap, "\nsector-size ");
    put_dec(buf, &n, cap, (uint64_t)d->sector_size);
    put(buf, &n, cap, "\nsectors ");
    put_dec(buf, &n, cap, d->sectors);
    put(buf, &n, cap, "\nwindow-sectors ");
    put_dec(buf, &n, cap, (uint64_t)d->window_sectors);
    put(buf, &n, cap, "\nraw-batch-sectors ");
    put_dec(buf, &n, cap, (uint64_t)DEVFS_RAW_BATCH_SECTORS);
    put(buf, &n, cap, "\nraw-driver-calls ");
    put_dec(buf, &n, cap, devfs_raw_calls());
    put(buf, &n, cap, "\nmbr-signature ");
    if (d->mbr_state == DISKINFO_MBR_OK) {
        put(buf, &n, cap, "ok");
    } else if (d->mbr_state == DISKINFO_MBR_NONE) {
        put(buf, &n, cap, "none");
    } else {
        put(buf, &n, cap, "unread");
    }
    put(buf, &n, cap, "\nmbr-id ");
    put_hex(buf, &n, cap, d->mbr_id, 8);
    put(buf, &n, cap, "\n");

    for (i = 0; i < MBR_PART_COUNT; i++) {
        put(buf, &n, cap, "part");
        put_dec(buf, &n, cap, (uint64_t)(i + 1));
        put(buf, &n, cap, " ");
        put_hex(buf, &n, cap, (uint32_t)d->parts[i].type, 2);
        put(buf, &n, cap, " ");
        put_dec(buf, &n, cap, (uint64_t)d->parts[i].start_lba);
        put(buf, &n, cap, " ");
        put_dec(buf, &n, cap, (uint64_t)d->parts[i].sector_count);
        put(buf, &n, cap, "\n");
    }
    buf[n] = '\0';
    return n;
}

void diskinfo_record_identify(const char *driver, int port, const volatile uint16_t *id,
                              uint64_t sectors)
{
    info.driver = driver;
    info.port = port;
    info.sectors = sectors;
    diskinfo_ata_string(info.serial, DISKINFO_SERIAL_CHARS, id, 10);
    diskinfo_ata_string(info.firmware, DISKINFO_FIRMWARE_CHARS, id, 23);
    diskinfo_ata_string(info.model, DISKINFO_MODEL_CHARS, id, 27);
    identify_seen = 1;
}

/** The 32-bit disk signature at offset 440 of the first sector. */
static uint32_t mbr_disk_id(const uint8_t *sector)
{
    return (uint32_t)sector[440] | ((uint32_t)sector[441] << 8) | ((uint32_t)sector[442] << 16) |
           ((uint32_t)sector[443] << 24);
}

int diskinfo_probe(int mounted)
{
    static uint8_t sector[BLOCK_SECTOR_SIZE];
    int i;

    info.mounted = mounted;
    /* Adopt a device the mount already published instead of probing again: a
     * second bring-up would reset the controller under a mounted file
     * system. */
    if (!block0 && block_ready() != 0) {
        info.state = DISKINFO_NO_DEVICE;
        info.driver = "none";
        return -1;
    }
    if (!block0) {
        info.state = DISKINFO_NO_DEVICE;
        info.driver = "none";
        return -1;
    }

    info.state = DISKINFO_PRESENT;
    if (!identify_seen) {
        info.driver = "unknown";
        info.port = -1;
        info.sectors = block0->sector_count;
    }
    info.sector_size = block0->sector_size;
    info.window_sectors = diskinfo_window(block0->sector_count);

    for (i = 0; i < MBR_PART_COUNT; i++) {
        info.parts[i].type = 0;
        info.parts[i].start_lba = 0;
        info.parts[i].sector_count = 0;
    }
    if (block0->read(block0, 0, 1, sector) != 0) {
        info.mbr_state = DISKINFO_MBR_UNREAD;
    } else if (mbr_parse(sector, info.parts) != 0) {
        info.mbr_state = DISKINFO_MBR_NONE;
        info.mbr_id = mbr_disk_id(sector);
    } else {
        info.mbr_state = DISKINFO_MBR_OK;
        info.mbr_id = mbr_disk_id(sector);
    }

    devfs_disk_publish(block0, info.window_sectors);
    klog(KLOG_INFO, "disk", "%s: %d sectors, model %s", info.driver,
         (uint32_t)info.sectors,
                  info.model);
    return 0;
}

const struct diskinfo *diskinfo_get(void)
{
    return &info;
}
