/*
 * usbdisk.c: the stick as a disk, from its arrival to the last hold on it.
 *
 * Three things happen here. A stick that has finished bring-up gets the
 * lowest free number and its partition table read, which is where a GUID
 * table and an MBR are told apart and where an unpartitioned volume is left
 * as the whole node alone. A stick that has gone stops being served and its
 * cache buffers are dealt with. And a stick that has gone and that nothing
 * holds any more is reclaimed: its number, its nodes and the class driver's
 * slot all go back.
 *
 * The table is read from the pump, where there is no process to put to
 * sleep, so it is read the way the pump does everything: one request is
 * started, and asked after on the passes that follow. Publishing the nodes
 * and reclaiming a stick touch what a process inside a syscall may be in
 * the middle of changing, so those run at a syscall entry, before the
 * dispatch, or on a pump pass that did not interrupt a syscall. On this one
 * processor a syscall is not preempted, so no count is half updated at
 * either moment; and a process only ever sleeps at a point where each thing
 * it holds has already been recorded in a count `usbdisk_holds` reads, so
 * the processes asleep inside syscalls are counted exactly.
 */

#include "usbdisk.h"

#include "../fs/bcache.h"
#include "../fs/devfs.h"
#include "../fs/vfs.h"
#include "../klog.h"
#include "../lib/kerrno.h"
#include "../lib/kirq.h"
#include "../proc/process.h"
#include "../usb/msc.h"
#include "block.h"
#include "gpt.h"
#include "mbr.h"

/* What a numbered stick is doing. A number is free in NONE and taken in
 * every other state. */
#define DISK_NONE     0
#define DISK_ARRIVED  1 /* up, and its table not read yet */
#define DISK_SCANNING 2 /* its table being read */
#define DISK_SCANNED  3 /* its table in hand, its nodes not published yet */
#define DISK_LIVE     4 /* published */
#define DISK_CLOSING  5 /* gone; the holds are being waited out */

/* Where the reading of a table stands. */
#define SCAN_SECTOR0 0
#define SCAN_HEADER  1
#define SCAN_ARRAY   2

struct usbdisk {
    int state;
    int slot;    /* the class driver's slot */
    int parts;   /* how many partitions were published */
    int refused; /* how many table entries were passed over */
    int marked;  /* the cache has been told the device has gone */
    uint64_t sectors;

    /* The reading of the table. */
    int scan;
    struct block_request *request;
    uint32_t generation;
    uint32_t array_lba;
    uint32_t array_sectors;
    uint32_t array_read;
    uint32_t array_bytes;
    uint32_t array_crc;
    uint32_t entry_count;
    uint32_t entry_size;
    uint64_t first_usable;
    uint64_t last_usable;
    const char *table;
    struct gpt_part found[GPT_PART_COUNT];
    int taken;
};

static struct usbdisk disks[USBDISK_MAX];
static uint32_t teardowns;

/* Set when a stick is waiting to be published or reclaimed; read by the
 * syscall entry and by the pump. */
static volatile int pending;

/* One table is read at a time, through these two buffers. The array is
 * sixteen kilobytes, which is not for a stack, and the second stick waits
 * for the first rather than needing a second pair. */
static int reading = -1;
static uint8_t scan_sector[BLOCK_SECTOR_SIZE];
static uint8_t table_array[GPT_MAX_ARRAY_BYTES];

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
    disks[n].slot = slot;
    disks[n].parts = 0;
    disks[n].refused = 0;
    disks[n].marked = 0;
    disks[n].sectors = 0;
    disks[n].request = 0;
    disks[n].taken = 0;
    disks[n].table = "none";
    disks[n].state = DISK_ARRIVED;
}

void usbdisk_left(int slot)
{
    int n;
    int named = 0;

    for (n = 0; n < USBDISK_MAX; n++) {
        if (disks[n].state != DISK_NONE && disks[n].slot == slot) {
            named = 1;
            if (disks[n].state != DISK_CLOSING) {
                disks[n].state = DISK_CLOSING;
                pending = 1;
            }
        }
    }
    /* A stick that went before it was up was never named: nothing holds it,
     * and the class driver's slot goes back here, or it would be held for
     * the life of the machine. */
    if (!named) {
        msc_release(slot);
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
    /* The table is read through the whole device, not a view; a read of
     * it quarantined by the pool still has the device's page. */
    held += block_device_holds(msc_block_device(disks[n].slot));
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

/** Whether `sector` looks like the boot sector of a volume with no table. */
static int volume_boot_sector(const uint8_t *sector)
{
    if (sector[0] == 0xEB || sector[0] == 0xE9) {
        return 1; /* a jump, which every FAT boot sector opens with */
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

/** Start reading `count` sectors at `lba` of the stick numbered `n`. */
static int scan_begin(int n, uint64_t lba, uint32_t count)
{
    block_device_t *dev = msc_block_device(disks[n].slot);

    if (dev == 0) {
        return -1;
    }
    disks[n].request = block_request_begin(dev, lba, count, &disks[n].generation);
    return disks[n].request == 0 ? -1 : 0;
}

/** The table is in hand, or could not be read: let the stick be published. */
static void scan_end(int n)
{
    reading = -1;
    disks[n].request = 0;
    if (disks[n].state == DISK_SCANNING) {
        disks[n].state = DISK_SCANNED;
        pending = 1;
    }
}

/** Judge the first sector and decide what to read next, if anything. */
static void scan_sector0(int n)
{
    struct usbdisk *d = &disks[n];
    struct mbr_part entries[MBR_PART_COUNT];

    if (gpt_protective(scan_sector)) {
        d->table = "guid";
        d->scan = SCAN_HEADER;
        if (scan_begin(n, 1, 1) != 0) {
            klog(KLOG_WARN, "usbdisk", "usb%d: its guid header could not be asked for", n);
            scan_end(n);
        }
        return;
    }
    if (mbr_parse(scan_sector, entries) == 0) {
        /* A volume with no table has the same signature an MBR has, with
         * four empty entries where the table would be; what tells the two
         * apart is the boot sector in front of them. */
        d->taken = take_mbr(entries, d->sectors, d->found, &d->refused);
        d->table = (d->taken == 0 && volume_boot_sector(scan_sector)) ? "one volume" : "mbr";
    } else if (volume_boot_sector(scan_sector)) {
        d->table = "one volume";
    }
    scan_end(n);
}

/**
 * Judge the GUID header and start on the array.
 *
 * The header is judged before anything it says is used, and the product of
 * the count and the size it gives is bounded there, because that product is
 * what the array is read by. The array is read whole, a page at a time, and
 * only checked and looked at once all of it is in hand.
 */
static void scan_header(int n)
{
    struct usbdisk *d = &disks[n];
    uint64_t array_lba;

    if (gpt_header_ok(scan_sector, 1, d->sectors) != 0) {
        klog(KLOG_WARN, "usbdisk", "usb%d: its guid header is not sound; no partitions published",
             n);
        scan_end(n);
        return;
    }
    d->entry_count = gpt_entry_count(scan_sector);
    d->entry_size = gpt_entry_size(scan_sector);
    /* The product is bounded by the header check. */
    d->array_bytes = d->entry_count * d->entry_size;
    d->array_sectors = (d->array_bytes + BLOCK_SECTOR_SIZE - 1u) / BLOCK_SECTOR_SIZE;
    d->array_crc = gpt_array_crc(scan_sector);
    d->first_usable = gpt_first_usable(scan_sector);
    d->last_usable = gpt_last_usable(scan_sector);
    array_lba = gpt_array_lba(scan_sector);
    if (array_lba > 0xFFFFFFFFu) {
        klog(KLOG_WARN, "usbdisk", "usb%d: its entry array is past a ten-byte read", n);
        scan_end(n);
        return;
    }
    d->array_lba = (uint32_t)array_lba;
    d->array_read = 0;
    d->scan = SCAN_ARRAY;
    if (scan_begin(n, d->array_lba,
                   d->array_sectors < BLOCK_REQ_SECTORS ? d->array_sectors : BLOCK_REQ_SECTORS) !=
        0) {
        klog(KLOG_WARN, "usbdisk", "usb%d: its entry array could not be asked for", n);
        scan_end(n);
    }
}

/** One more page of the array is in; take the next, or judge the whole. */
static void scan_array(int n, uint32_t got)
{
    struct usbdisk *d = &disks[n];
    uint32_t left;

    d->array_read += got;
    left = d->array_sectors - d->array_read;
    if (left != 0) {
        if (scan_begin(n, d->array_lba + d->array_read,
                       left < BLOCK_REQ_SECTORS ? left : BLOCK_REQ_SECTORS) != 0) {
            klog(KLOG_WARN, "usbdisk", "usb%d: its entry array could not be asked for", n);
            scan_end(n);
        }
        return;
    }
    if (gpt_crc32(table_array, d->array_bytes) != d->array_crc) {
        klog(KLOG_WARN, "usbdisk", "usb%d: its entry array does not match its checksum", n);
        scan_end(n);
        return;
    }
    d->taken = gpt_parse_entries(table_array, d->entry_count, d->entry_size, d->first_usable,
                                 d->last_usable, d->found, &d->refused);
    scan_end(n);
}

/**
 * Advance the table being read by one step, if its request has ended.
 *
 * A stick that went while its table was being read has its request failed
 * by the class driver; the answer is taken here and dropped, so the slot goes
 * back to the pool, and the stick goes on to be reclaimed like any other.
 */
static void scan_advance(void)
{
    int n;
    struct usbdisk *d;
    int status;
    uint32_t got;

    if (reading < 0) {
        for (n = 0; n < USBDISK_MAX; n++) {
            if (disks[n].state == DISK_ARRIVED) {
                block_device_t *dev = msc_block_device(disks[n].slot);

                if (dev == 0 || !msc_ready(disks[n].slot)) {
                    disks[n].state = DISK_CLOSING;
                    pending = 1;
                    continue;
                }
                d = &disks[n];
                d->sectors = dev->sector_count;
                d->scan = SCAN_SECTOR0;
                d->state = DISK_SCANNING;
                reading = n;
                if (scan_begin(n, 0, 1) != 0) {
                    klog(KLOG_WARN, "usbdisk", "usb%d: its first sector could not be asked for", n);
                    scan_end(n);
                }
                return;
            }
        }
        return;
    }

    n = reading;
    d = &disks[n];
    if (d->request == 0) {
        scan_end(n);
        return;
    }
    got = d->request->count;
    if (!block_request_ended(d->request, d->generation,
                             d->scan == SCAN_ARRAY ? table_array + d->array_read * BLOCK_SECTOR_SIZE
                                                   : scan_sector,
                             &status)) {
        return;
    }
    d->request = 0;
    if (d->state != DISK_SCANNING) {
        scan_end(n); /* it went while being read; nothing of it is published */
        return;
    }
    if (status != 0) {
        klog(KLOG_WARN, "usbdisk", "usb%d: a sector of its table could not be read", n);
        scan_end(n);
        return;
    }
    switch (d->scan) {
    case SCAN_SECTOR0:
        scan_sector0(n);
        return;
    case SCAN_HEADER:
        scan_header(n);
        return;
    default:
        scan_array(n, got);
        return;
    }
}

/** Publish the stick numbered `n` from the table read for it. */
static void publish(int n)
{
    struct usbdisk *d = &disks[n];
    block_device_t *dev = msc_block_device(d->slot);
    uint32_t flags;

    if (dev == 0) {
        d->state = DISK_CLOSING;
        return;
    }
    devfs_usb_publish(n, dev, d->found, d->taken);
    d->parts = d->taken;
    /* The stick may have gone between the reading and this; a departure
     * arrives from the pump and must not be written over. */
    flags = kirq_save();
    if (d->state == DISK_SCANNED) {
        d->state = DISK_LIVE;
    }
    kirq_restore(flags);
    klog(KLOG_INFO, "usbdisk", "usb%d: %s: %d sectors, %d partitions, %d passed over", n, d->table,
         (int)d->sectors, d->taken, d->refused);
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

/**
 * Publish what has been read and reclaim what nothing holds.
 *
 * Runs at the syscall entry, before the dispatch, or from the pump on a
 * pass that did not interrupt a syscall. No count is half updated at either
 * moment, since a syscall is not preempted, and a process asleep inside one
 * is at a point where everything it holds has been recorded in a count
 * `usbdisk_holds` reads.
 */
static void settle(void)
{
    int n;
    int left;
    uint32_t flags;

    for (n = 0; n < USBDISK_MAX; n++) {
        if (disks[n].state == DISK_SCANNED) {
            publish(n);
        }
    }
    for (n = 0; n < USBDISK_MAX; n++) {
        if (disks[n].state != DISK_CLOSING) {
            continue;
        }
        if (!disks[n].marked) {
            close_down(n);
        }
        /* A read of its table still out is a hold of its own: the completion
         * is addressed to that pool slot, and the slot records the device. */
        if (reading == n || usbdisk_holds(n) != 0) {
            continue;
        }
        reclaim(n);
    }
    /* Whether anything is still waiting is decided with interrupts off and
     * from the states themselves, not from what this pass saw: a stick can
     * finish its table or go while this runs, and its `pending` must not be
     * written over. */
    flags = kirq_save();
    left = 0;
    for (n = 0; n < USBDISK_MAX; n++) {
        if (disks[n].state == DISK_SCANNED || disks[n].state == DISK_CLOSING) {
            left++;
        }
    }
    pending = left != 0;
    kirq_restore(flags);
}

void usbdisk_step(void)
{
    scan_advance();
    if (pending && !proc_in_syscall()) {
        settle();
    }
}

void usbdisk_service(void)
{
    if (pending) {
        settle();
    }
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
    at = put(out, size, at,
             disks[n].state == DISK_CLOSING ? "closing"
             : disks[n].state == DISK_LIVE  ? "ready"
                                            : "reading");
    at = put(out, size, at, " flushes ");
    at = putnum(out, size, at, ident.flushes);
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
