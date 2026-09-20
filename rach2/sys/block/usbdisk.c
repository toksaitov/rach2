/*
 * usbdisk.c: a stick as a disk of the registry, from its arrival to the
 * last hold on it.
 *
 * Three things happen here. A logical unit the class driver has brought up
 * is registered as a disk, given the lowest free number, and has its
 * partition table read by the partition service, as a SATA disk is; the
 * sectors the service asks for come through the pump, a request at a time,
 * since there is no process to put to sleep there. A stick that has gone
 * stops being served, its nodes are hidden and its cache buffers are dealt
 * with. And a stick that has gone and that nothing holds any more is
 * reclaimed: its number goes back, its node slots go back to devfs for the
 * next stick to take, and the class driver's transport slot goes back when
 * no other logical unit of the same stick still uses it.
 *
 * Publishing and reclaiming touch what a process inside a syscall may be in
 * the middle of changing, so those run at a syscall entry, before the
 * dispatch, or on a pump pass that did not interrupt a syscall. Reading a
 * partition table is not one of those: the scan advances on every pump pass,
 * since it touches only its own stick. On this one
 * processor a syscall is not preempted, so no count is half updated at
 * either moment; and a process only ever sleeps at a point where each thing
 * it holds has already been recorded in a count `usbdisk_holds` reads, so
 * the processes asleep inside syscalls are counted exactly.
 */

#include "usbdisk.h"
#include "loop.h"
#include "../fs/bcache.h"
#include "../fs/devfs.h"
#include "../fs/vfs.h"
#include "../klog.h"
#include "../lib/kerrno.h"
#include "../lib/kirq.h"
#include "../lib/kstring.h"
#include "../proc/process.h"
#include "../usb/msc.h"
#include "block.h"
#include "disk.h"
#include "diskinfo.h"

/* What a stick is doing. A record is free in NONE and taken in every other
 * state. */
#define STICK_NONE     0
#define STICK_ARRIVED  1 /* up, not yet registered */
#define STICK_SCANNING 2 /* registered, its table being read */
#define STICK_LIVE     3 /* published */
#define STICK_CLOSING  4 /* gone; the holds are being waited out */

struct usbdisk {
    int state;
    int slot;   /* the class driver's slot */
    int lun;    /* the logical unit on it */
    int marked; /* the cache has been told the device has gone */
    struct disk *disk;
};

static struct usbdisk sticks[USBDISK_MAX];
static uint32_t teardowns;

/* Set when a stick is waiting to be registered or reclaimed; read by the
 * syscall entry and by the pump. */
static volatile int pending;

void usbdisk_arrived(int slot, int lun)
{
    int n;

    for (n = 0; n < USBDISK_MAX; n++) {
        if (sticks[n].state == STICK_NONE) {
            break;
        }
    }
    if (n == USBDISK_MAX) {
        klog(KLOG_WARN, "usbdisk", "refused: slot %d lun %d: every record is taken", slot, lun);
        return;
    }
    sticks[n].slot = slot;
    sticks[n].lun = lun;
    sticks[n].marked = 0;
    sticks[n].disk = 0;
    sticks[n].state = STICK_ARRIVED;
    pending = 1;
}

void usbdisk_left(int slot)
{
    int n;
    int named = 0;

    for (n = 0; n < USBDISK_MAX; n++) {
        if (sticks[n].state != STICK_NONE && sticks[n].slot == slot) {
            named = 1;
            if (sticks[n].state != STICK_CLOSING) {
                sticks[n].state = STICK_CLOSING;
                pending = 1;
            }
        }
    }
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
    int free_records = 0;

    for (n = 0; n < USBDISK_MAX; n++) {
        if (sticks[n].state == STICK_NONE) {
            free_records++;
        }
    }
    return free_records;
}

/**
 * How many things hold stick `n`: descriptors open on its nodes, requests
 * the class driver's device still owns, and on every view the mounts, the
 * cache buffers and the requests. Zero for a record that has no disk yet.
 */
int usbdisk_holds(int n)
{
    struct disk *d;
    int k;
    int held = 0;

    if (n < 0 || n >= USBDISK_MAX || sticks[n].state == STICK_NONE || !sticks[n].disk) {
        return 0;
    }
    d = sticks[n].disk;
    held += d->opens;
    held += block_device_holds(d->dev);
    for (k = -1; k < d->table.count; k++) {
        block_device_t *view = k < 0 ? &d->whole.part.device : &d->views[k].part.device;

        held += vfs_device_holds(view);
        held += bcache_device_holds(view);
        held += block_device_holds(view);
    }
    return held;
}

/*
 * The reading of a stick's table, from the pump, where there is no process
 * to put to sleep. The partition service reads through a callback, so it
 * is given one that answers from a cache of sectors fetched the way the
 * pump does everything: a request started, and asked after on the passes
 * that follow. A pass of the service that asks for a sector the cache does
 * not have is a miss; the pass's result is dropped, the sector is fetched,
 * and the service runs again once it has arrived, until a pass asks for
 * nothing new. One table is read at a time, and a second stick waits.
 */
#define SCAN_CACHE_SECTORS 96 /* sector 0, two headers and arrays, the EBR chain */

static struct {
    int n; /* the record being read, -1 for none */
    int cached;
    uint64_t lba[SCAN_CACHE_SECTORS];
    uint8_t sector[SCAN_CACHE_SECTORS][BLOCK_SECTOR_SIZE];
    int miss;
    uint64_t miss_lba;
    uint32_t miss_count;
    struct block_request *request;
    uint32_t generation;
    uint64_t request_lba;
    uint32_t request_count;
    uint8_t work[PART_WORK_BYTES];
    struct part_table table;
    uint8_t got[BLOCK_REQ_SECTORS * BLOCK_SECTOR_SIZE]; /* what a finished request read */
} scan = {-1, 0, {0}, {{0}}, 0, 0, 0, 0, 0, 0, 0, {0}, {0}, {0}};

static int scan_read(void *ctx, uint64_t lba, uint32_t count, void *buf)
{
    uint8_t *out = (uint8_t *)buf;
    uint32_t i;

    (void)ctx;
    for (i = 0; i < count; i++) {
        int k, found = -1;

        for (k = 0; k < scan.cached; k++) {
            if (scan.lba[k] == lba + i) {
                found = k;
            }
        }
        if (found < 0) {
            if (!scan.miss) {
                scan.miss = 1;
                scan.miss_lba = lba + i;
                scan.miss_count = count - i;
                if (scan.miss_count > BLOCK_REQ_SECTORS) {
                    scan.miss_count = BLOCK_REQ_SECTORS;
                }
            }
            return -1;
        }
        k_memcpy(out + i * BLOCK_SECTOR_SIZE, scan.sector[found], BLOCK_SECTOR_SIZE);
    }
    return 0;
}

/** Run the service over the cache: 1 when it finished, 0 when a sector is
 * being fetched, -1 when the cache is full or the stick will not answer. */
static int scan_pass(int n)
{
    struct usbdisk *st = &sticks[n];
    block_device_t *dev = msc_lun_device(st->slot, st->lun);

    if (!dev) {
        return -1;
    }
    scan.miss = 0;
    (void)part_scan(&scan.table, scan_read, 0, dev->sector_count, scan.work);
    if (!scan.miss) {
        return 1;
    }
    if (scan.cached + (int)scan.miss_count > SCAN_CACHE_SECTORS) {
        klog(KLOG_WARN, "usbdisk", "slot %d lun %d: its table needs more sectors than are read",
             st->slot, st->lun);
        return -1;
    }
    scan.request = block_request_begin(dev, scan.miss_lba, scan.miss_count, &scan.generation);
    if (!scan.request) {
        return -1;
    }
    scan.request_lba = scan.miss_lba;
    scan.request_count = scan.miss_count;
    return 0;
}

/** The stick has been read: register the table, publish the nodes. */
static void scan_done(int n)
{
    struct usbdisk *st = &sticks[n];
    struct disk *d = st->disk;
    block_device_t *dev = msc_lun_device(st->slot, st->lun);

    scan.n = -1;
    if (!d || !dev) {
        st->state = STICK_CLOSING;
        return;
    }
    disk_set_table(d, &scan.table, dev);
    devfs_disk_publish(d, dev->sector_count);
    st->state = STICK_LIVE;
    klog(KLOG_INFO, "usbdisk", "disk%d: %s: %d MiB, %d partitions, %d passed over", d->number,
         disk_table_name(d), (int)(dev->sector_count / 2048), d->table.count, d->table.refused);
}

/** Start or advance the one scan in progress; the pump calls it every pass. */
static void scan_advance(void)
{
    int n, rc;

    if (scan.n < 0) {
        for (n = 0; n < USBDISK_MAX; n++) {
            if (sticks[n].state == STICK_SCANNING) {
                break;
            }
        }
        if (n == USBDISK_MAX) {
            return;
        }
        scan.n = n;
        scan.cached = 0;
        scan.request = 0;
    }
    n = scan.n;
    if (sticks[n].state != STICK_SCANNING) {
        /* Pulled while its table was being read: the request in flight is
         * the device's until it answers or until the deadline quarantines it,
         * so it is asked after until one of those, and only then is the scan
         * let go of. */
        int status = 0;

        if (scan.request &&
            !block_request_ended(scan.request, scan.generation, scan.got, &status)) {
            return;
        }
        scan.request = 0;
        scan.n = -1;
        return;
    }
    if (scan.request) {
        int status = 0;
        uint32_t i;

        if (!block_request_ended(scan.request, scan.generation, scan.got, &status)) {
            return;
        }
        scan.request = 0;
        if (status != 0) {
            klog(KLOG_WARN, "usbdisk", "slot %d lun %d: its table could not be read",
                 sticks[n].slot, sticks[n].lun);
            scan.table.kind = PART_UNREAD;
            scan.table.count = 0;
            scan_done(n);
            return;
        }
        for (i = 0; i < scan.request_count; i++) {
            scan.lba[scan.cached] = scan.request_lba + i;
            k_memcpy(scan.sector[scan.cached], scan.got + i * BLOCK_SECTOR_SIZE, BLOCK_SECTOR_SIZE);
            scan.cached++;
        }
    }
    rc = scan_pass(n);
    if (rc == 1) {
        scan_done(n);
    } else if (rc < 0) {
        scan.table.kind = PART_UNREAD;
        scan.table.count = 0;
        scan_done(n);
    }
}

/**
 * Register the unit that arrived as a disk and start reading its table.
 * Nothing here sleeps: the registry and the nodes are made in place, and
 * the table comes through the pump.
 */
static void bring_up(int n)
{
    struct usbdisk *s = &sticks[n];
    block_device_t *dev = msc_lun_device(s->slot, s->lun);
    struct msc_ident ident;
    struct disk *d;

    if (dev == 0) {
        s->state = STICK_CLOSING;
        return;
    }
    d = disk_register(dev, "usb", s->slot * MSC_MAX_LUNS + s->lun, 0);
    if (d == 0) {
        klog(KLOG_WARN, "usbdisk", "slot %d lun %d: the registry is full", s->slot, s->lun);
        s->state = STICK_CLOSING;
        return;
    }
    if (msc_lun_ident(s->slot, s->lun, &ident) == 0) {
        int i;

        disk_set_model(d, ident.vendor, ident.product);
        for (i = 0; i < DISK_SERIAL_MAX && ident.serial[i]; i++) {
            d->serial[i] = ident.serial[i];
        }
        d->serial[i] = '\0';
    }
    d->removable = 1;
    s->disk = d;
    s->state = STICK_SCANNING;
}

/** The stick has gone: hide its nodes and tell the caches. */
static void close_down(int n)
{
    struct disk *d = sticks[n].disk;
    int k;

    if (d) {
        d->present = 0;
        for (k = -1; k < d->table.count; k++) {
            block_device_t *view = k < 0 ? &d->whole.part.device : &d->views[k].part.device;

            loop_device_gone(view);
            bcache_device_gone(view);
        }
    }
    sticks[n].marked = 1;
}

/**
 * Nothing holds the stick any more: its number goes back, and its transport
 * slot with it when no other logical unit of the same stick still holds it.
 */
static void reclaim(int n)
{
    struct disk *d = sticks[n].disk;
    int number = d ? d->number : -1;
    int shared = 0;
    int k;

    if (d) {
        disk_release(d);
    }
    /* The slot is the transport, and a device with several logical units
     * has a record for each. Releasing it while another unit is still
     * held would leave that unit's holders addressing storage the next
     * device to arrive can take. */
    for (k = 0; k < USBDISK_MAX; k++) {
        if (k != n && sticks[k].state != STICK_NONE && sticks[k].slot == sticks[n].slot) {
            shared = 1;
        }
    }
    if (!shared) {
        msc_release(sticks[n].slot);
    }
    sticks[n].state = STICK_NONE;
    sticks[n].disk = 0;
    sticks[n].marked = 0;
    teardowns++;
    if (number >= 0) {
        klog(KLOG_INFO, "usbdisk", "disk%d: reclaimed", number);
    }
}

/**
 * Settle what is pending: register what arrived, close what has gone, reclaim
 * what nothing holds. A new stick's partition table is not read here; the
 * scan reads it over the pump passes that follow.
 */
static void settle(void)
{
    int n;
    int left;
    uint32_t flags;

    for (n = 0; n < USBDISK_MAX; n++) {
        if (sticks[n].state == STICK_ARRIVED) {
            bring_up(n);
        }
    }
    for (n = 0; n < USBDISK_MAX; n++) {
        if (sticks[n].state != STICK_CLOSING) {
            continue;
        }
        if (!sticks[n].marked) {
            close_down(n);
        }
        if (scan.n == n || usbdisk_holds(n) != 0) {
            continue;
        }
        reclaim(n);
    }
    flags = kirq_save();
    left = 0;
    for (n = 0; n < USBDISK_MAX; n++) {
        if (sticks[n].state == STICK_ARRIVED || sticks[n].state == STICK_CLOSING) {
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

static int put(char *out, int size, int at, const char *text)
{
    while (*text && at < size - 1) {
        out[at++] = *text++;
    }
    return at;
}

/** Append a 64-bit sector count, which a stick past 2 TiB has. */
static int put_sectors(char *out, int size, int at, uint64_t value)
{
    char digits[24];

    if (diskinfo_u64_dec(digits, (int)sizeof(digits), value) > 0) {
        at = put(out, size, at, digits);
    }
    return at;
}

static int putnum(char *out, int size, int at, uint32_t value)
{
    char digits[12];
    int n = 0;

    do {
        digits[n++] = (char)('0' + value % 10);
        value /= 10;
    } while (value > 0);
    while (n > 0 && at < size - 1) {
        out[at++] = digits[--n];
    }
    return at;
}

int usbdisk_describe(int n, char *out, int size)
{
    struct msc_ident ident;
    struct disk *d;
    int at = 0;

    if (n < 0 || n >= USBDISK_MAX || out == 0 || size < 64) {
        return 0;
    }
    if (sticks[n].state == STICK_NONE) {
        return 0;
    }
    if (msc_lun_ident(sticks[n].slot, sticks[n].lun, &ident) != 0) {
        return 0;
    }
    d = sticks[n].disk;
    at = put(out, size, at, "disk");
    if (d) {
        at = putnum(out, size, at, (uint32_t)d->number);
    } else {
        at = put(out, size, at, "?");
    }
    at = put(out, size, at, " ");
    at = put(out, size, at, ident.vendor[0] ? ident.vendor : "?");
    at = put(out, size, at, " ");
    at = put(out, size, at, ident.product[0] ? ident.product : "?");
    at = put(out, size, at, " ");
    at = put_sectors(out, size, at, ident.blocks);
    at = put(out, size, at, " sectors ");
    at = putnum(out, size, at, d ? (uint32_t)d->table.count : 0u);
    at = put(out, size, at, " parts ");
    at = put(out, size, at,
             sticks[n].state == STICK_CLOSING ? "closing"
             : sticks[n].state == STICK_LIVE  ? "ready"
                                              : "reading");
    at = put(out, size, at, " flushes ");
    at = putnum(out, size, at, ident.flushes);
    if (ident.write_protect) {
        at = put(out, size, at, " ro");
    }
    if (sticks[n].state == STICK_CLOSING) {
        at = put(out, size, at, " holds ");
        at = putnum(out, size, at, (uint32_t)usbdisk_holds(n));
    }
    at = put(out, size, at, "\n");
    out[at] = '\0';
    return at;
}
