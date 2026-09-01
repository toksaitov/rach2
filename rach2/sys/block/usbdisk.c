/*
 * usbdisk.c: the stick as a disk, from its arrival to the last hold on it.
 *
 * Three things happen here. A stick that has finished bring-up gets the
 * lowest free number and its partition table read, which is where a GUID
 * table and an MBR are told apart and where an unpartitioned volume is left
 * as the whole node alone. A stick that has gone stops being served and its
 * cache buffers are dealt with. And a stick that has gone and that nothing
 * holds any more is reclaimed: its number, its nodes and the class driver's
 * slot all go back, so a stick plugged and pulled a hundred times exhausts
 * nothing.
 *
 * The reading happens in a syscall, not in the pump. A transfer to a stick
 * sleeps its caller, and the pump runs in an interrupt; the pump therefore
 * records what happened and the next process into the kernel does the work.
 */

#include "usbdisk.h"

#include "../fs/bcache.h"
#include "../fs/devfs.h"
#include "../fs/vfs.h"
#include "../klog.h"
#include "../lib/kerrno.h"
#include "../lib/kstring.h"
#include "../usb/msc.h"
#include "block.h"
#include "gpt.h"
#include "mbr.h"

/* What a numbered stick is doing. A number is free in NONE and taken in
 * every other state. */
#define DISK_NONE    0
#define DISK_ARRIVED 1 /* up, and its table not read yet */
#define DISK_LIVE    2 /* published */
#define DISK_CLOSING 3 /* gone; the holds are being waited out */

struct usbdisk {
    int state;
    int slot;    /* the class driver's slot */
    int parts;   /* how many partitions were published */
    int refused; /* how many table entries were passed over */
    int marked;  /* the cache has been told the device has gone */
    uint64_t sectors;
};

static struct usbdisk disks[USBDISK_MAX];
static uint32_t teardowns;

/* Set by the pump, read by the syscall entry. The work behind an arrival and
 * a departure both sleep, so neither is done where it is learned. */
static volatile int pending;

/* One scan at a time. The service runs from a syscall and sleeps inside a
 * transfer, so a second process can enter it while the first is asleep in
 * the middle of a table; the second must not read into the same buffers. */
static int scanning;

/* The sector and the entry array a scan reads through. Both are here rather
 * than on the stack because a kernel stack is four pages and the array is
 * sixteen kilobytes, and because only one scan runs at a time. */
static uint8_t scan_sector[BLOCK_SECTOR_SIZE];
static uint8_t scan_array[GPT_MAX_ARRAY_BYTES];

void usbdisk_arrived(int slot)
{
    int n;

    for (n = 0; n < USBDISK_MAX; n++) {
        if (disks[n].state == DISK_NONE) {
            break;
        }
    }
    if (n == USBDISK_MAX) {
        klog(KLOG_WARN, "usbdisk", "refused: slot %d: every name is taken", slot);
        return;
    }
    disks[n].state = DISK_ARRIVED;
    disks[n].slot = slot;
    disks[n].parts = 0;
    disks[n].refused = 0;
    disks[n].marked = 0;
    disks[n].sectors = 0;
    pending = 1;
}

void usbdisk_left(int slot)
{
    int n;

    for (n = 0; n < USBDISK_MAX; n++) {
        if (disks[n].state != DISK_NONE && disks[n].state != DISK_CLOSING &&
            disks[n].slot == slot) {
            disks[n].state = DISK_CLOSING;
            pending = 1;
        }
    }
}

uint32_t usbdisk_teardowns(void)
{
    return teardowns;
}

int usbdisk_free(void)
{
    int n;
    int free_names = 0;

    for (n = 0; n < USBDISK_MAX; n++) {
        if (disks[n].state == DISK_NONE) {
            free_names++;
        }
    }
    return free_names;
}

int usbdisk_holds(int n)
{
    int k;
    int held = 0;

    if (n < 0 || n >= USBDISK_MAX || disks[n].state == DISK_NONE) {
        return 0;
    }
    held += devfs_usb_opens(n);
    for (k = 0; k <= GPT_PART_COUNT; k++) {
        block_device_t *view = devfs_usb_view(n, k);

        if (view == 0) {
            continue;
        }
        held += vfs_device_holds(view);
        held += bcache_device_holds(view);
        held += block_device_holds(view);
    }
    return held;
}

/**
 * Read `count` sectors at `lba` from the stick into `into`.
 *
 * Every read here is a table, so a failure is not retried: a stick that
 * cannot give its first sector has nothing this layer can publish, and the
 * whole node is left for a reader to make of what it will.
 */
static int scan_read(block_device_t *dev, uint64_t lba, uint32_t count, uint8_t *into)
{
    return block_request(dev, 0, lba, count, into);
}

/** Whether `sector` looks like the boot sector of a volume with no table. */
static int volume_boot_sector(const uint8_t *sector)
{
    if (sector[0] == 0xEB || sector[0] == 0xE9) {
        return 1; /* a jump instruction, which every FAT boot sector opens with */
    }
    return sector[3] == 'E' && sector[4] == 'X' && sector[5] == 'F' && sector[6] == 'A' &&
           sector[7] == 'T';
}

/**
 * Take the MBR entries that can be published.
 *
 * `mbr_parse` reads the four entries as they are stored and judges none of
 * them, because what makes an entry publishable is the medium it is on. An
 * entry that is empty, that runs past the stick, or that overlaps one
 * already taken is passed over with the rest still read, and how many were
 * passed over is reported.
 */
static int take_mbr(const struct mbr_part *entries, uint64_t sectors,
                    struct gpt_part parts[GPT_PART_COUNT], int *refused)
{
    int i;
    int taken = 0;
    int passed_over = 0;

    for (i = 0; i < GPT_PART_COUNT; i++) {
        parts[i].start_lba = 0;
        parts[i].sector_count = 0;
    }
    for (i = 0; i < MBR_PART_COUNT; i++) {
        uint64_t first = entries[i].start_lba;
        uint64_t count = entries[i].sector_count;
        int j;
        int overlaps = 0;

        if (entries[i].type == 0 || count == 0) {
            continue;
        }
        if (first == 0 || first + count > sectors) {
            passed_over++;
            continue;
        }
        for (j = 0; j < taken; j++) {
            uint64_t other = parts[j].start_lba;

            if (first < other + parts[j].sector_count && other < first + count) {
                overlaps = 1;
            }
        }
        if (overlaps) {
            passed_over++;
            continue;
        }
        parts[taken].start_lba = first;
        parts[taken].sector_count = count;
        taken++;
    }
    *refused = passed_over;
    return taken;
}

/**
 * Read a GUID table and take what it publishes.
 *
 * The header is judged before anything it says is used, and the entry array
 * is read whole and checked against the checksum in the header before a
 * single entry is looked at. An array is at most sixteen kilobytes, so
 * reading it whole costs one transfer of thirty-two sectors, and reading it
 * four entries at a time would take a table whose fifth entry is rubbish for
 * a table that is sound.
 */
static int take_gpt(block_device_t *dev, uint64_t sectors, struct gpt_part parts[GPT_PART_COUNT],
                    int *refused)
{
    uint32_t count;
    uint32_t entry_size;
    uint32_t bytes;
    uint32_t array_sectors;

    if (scan_read(dev, 1, 1, scan_sector) != 0) {
        klog(KLOG_WARN, "usbdisk", "its guid header could not be read");
        return 0;
    }
    if (gpt_header_ok(scan_sector, 1, sectors) != 0) {
        klog(KLOG_WARN, "usbdisk", "its guid header is not sound; no partitions published");
        return 0;
    }
    count = gpt_entry_count(scan_sector);
    entry_size = gpt_entry_size(scan_sector);
    bytes = count * entry_size; /* bounded by the header check */
    array_sectors = (bytes + BLOCK_SECTOR_SIZE - 1u) / BLOCK_SECTOR_SIZE;
    {
        uint64_t array_lba = gpt_array_lba(scan_sector);
        uint32_t stored = gpt_array_crc(scan_sector);
        uint64_t first_usable = gpt_first_usable(scan_sector);
        uint64_t last_usable = gpt_last_usable(scan_sector);

        if (array_lba + array_sectors > sectors) {
            klog(KLOG_WARN, "usbdisk", "its entry array runs past the medium");
            return 0;
        }
        if (scan_read(dev, array_lba, array_sectors, scan_array) != 0) {
            klog(KLOG_WARN, "usbdisk", "its entry array could not be read");
            return 0;
        }
        if (gpt_crc32(scan_array, bytes) != stored) {
            klog(KLOG_WARN, "usbdisk", "its entry array does not match its checksum");
            return 0;
        }
        return gpt_parse_entries(scan_array, count, entry_size, first_usable, last_usable, parts,
                                 refused);
    }
}

/** Read the table of the stick numbered `n` and publish what it describes. */
static void scan(int n)
{
    block_device_t *dev = msc_block_device(disks[n].slot);
    struct gpt_part parts[GPT_PART_COUNT];
    struct mbr_part entries[MBR_PART_COUNT];
    const char *table = "none";
    int taken = 0;
    int refused = 0;
    int i;

    if (dev == 0 || !msc_ready(disks[n].slot)) {
        disks[n].state = DISK_CLOSING;
        return;
    }
    for (i = 0; i < GPT_PART_COUNT; i++) {
        parts[i].start_lba = 0;
        parts[i].sector_count = 0;
    }
    disks[n].sectors = dev->sector_count;

    if (scan_read(dev, 0, 1, scan_sector) != 0) {
        klog(KLOG_WARN, "usbdisk", "usb%d: its first sector could not be read", n);
    } else if (gpt_protective(scan_sector)) {
        table = "guid";
        taken = take_gpt(dev, dev->sector_count, parts, &refused);
    } else if (mbr_parse(scan_sector, entries) == 0) {
        table = "mbr";
        taken = take_mbr(entries, dev->sector_count, parts, &refused);
    } else if (volume_boot_sector(scan_sector)) {
        table = "one volume";
    }

    devfs_usb_publish(n, dev, parts, taken);
    disks[n].parts = taken;
    disks[n].refused = refused;
    disks[n].state = DISK_LIVE;
    klog(KLOG_INFO, "usbdisk", "usb%d: %s: %d sectors, %d partitions, %d passed over", n, table,
         (int)dev->sector_count, taken, refused);
}

/**
 * Give up the stick numbered `n`.
 *
 * The nodes go first, so nothing new can open them, and then the cache is
 * told: a clean buffer nobody holds is dropped, a dirty one has nowhere to
 * be written and takes the mounts read-only, and one a sleeping process
 * holds is marked to be dropped when that process lets it go, because a
 * buffer cannot be reused under its holder.
 */
static void close_down(int n)
{
    int k;

    devfs_usb_hide(n);
    for (k = 0; k <= GPT_PART_COUNT; k++) {
        block_device_t *view = devfs_usb_view(n, k);

        if (view != 0) {
            bcache_device_gone(view);
        }
    }
    disks[n].marked = 1;
}

/** Take back the number, the nodes and the class driver's slot. */
static void reclaim(int n)
{
    devfs_usb_withdraw(n);
    msc_release(disks[n].slot);
    disks[n].state = DISK_NONE;
    disks[n].parts = 0;
    disks[n].refused = 0;
    disks[n].marked = 0;
    disks[n].sectors = 0;
    teardowns++;
    klog(KLOG_INFO, "usbdisk", "usb%d: reclaimed", n);
}

void usbdisk_service(void)
{
    int n;
    int left = 0;

    if (!pending || scanning) {
        return;
    }
    scanning = 1;
    for (n = 0; n < USBDISK_MAX; n++) {
        if (disks[n].state == DISK_ARRIVED) {
            scan(n);
        }
    }
    for (n = 0; n < USBDISK_MAX; n++) {
        if (disks[n].state != DISK_CLOSING) {
            continue;
        }
        if (!disks[n].marked) {
            close_down(n);
        }
        if (usbdisk_holds(n) == 0) {
            reclaim(n);
        } else {
            left++;
        }
    }
    scanning = 0;
    /* A stick still held is looked at again on the next syscall, which is
     * where the release that frees it will have happened. */
    pending = left != 0;
}

/** Append a string, bounded, and answer where the next one goes. */
static int put(char *out, int size, int at, const char *text)
{
    while (*text && at < size - 1) {
        out[at++] = *text++;
    }
    return at;
}

/** Append a decimal number, bounded. */
static int putnum(char *out, int size, int at, uint32_t value)
{
    char digits[12];
    int n = 0;

    if (value == 0) {
        digits[n++] = '0';
    }
    while (value != 0) {
        digits[n++] = (char)('0' + (value % 10u));
        value /= 10u;
    }
    while (n > 0 && at < size - 1) {
        out[at++] = digits[--n];
    }
    return at;
}

int usbdisk_describe(int n, char *out, int size)
{
    struct msc_ident ident;
    int at = 0;

    if (n < 0 || n >= USBDISK_MAX || out == 0 || size < 64) {
        return 0;
    }
    if (disks[n].state == DISK_NONE) {
        return 0;
    }
    if (msc_ident(disks[n].slot, &ident) != 0) {
        return 0;
    }
    at = put(out, size, at, "usb");
    at = putnum(out, size, at, (uint32_t)n);
    at = put(out, size, at, " ");
    at = put(out, size, at, ident.vendor[0] ? ident.vendor : "?");
    at = put(out, size, at, " ");
    at = put(out, size, at, ident.product[0] ? ident.product : "?");
    at = put(out, size, at, " ");
    at = putnum(out, size, at, (uint32_t)disks[n].sectors);
    at = put(out, size, at, " sectors ");
    at = putnum(out, size, at, (uint32_t)disks[n].parts);
    at = put(out, size, at, " parts ");
    at = put(out, size, at, disks[n].state == DISK_CLOSING ? "closing" : "ready");
    if (ident.write_protect) {
        at = put(out, size, at, " ro");
    }
    if (disks[n].state == DISK_CLOSING) {
        at = put(out, size, at, " holds ");
        at = putnum(out, size, at, (uint32_t)usbdisk_holds(n));
    }
    at = put(out, size, at, "\n");
    out[at] = '\0';
    return at;
}
