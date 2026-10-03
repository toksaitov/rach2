/*
 * usbdisk_hosttest.c: a stick as a disk of the registry, from its arrival
 * to its last hold.
 *
 * host-deps: rach2/sys/block/disk.c rach2/sys/block/partition.c
 * host-deps: rach2/sys/block/gpt.c rach2/sys/lib/kstring.c
 * host-deps: rach2/sys/lib/kctypes.c
 *
 * The class driver, the block request layer, devfs and the caches are
 * stubs: a medium in memory answers the requests the scan starts, one per
 * pass of the pump, and counters stand for the holds. What is judged is
 * the layer's own work: a unit that arrives is registered with the lowest
 * free number and its table read through the pump, a GUID table across
 * several requests; a stick that is pulled is closed at once and reclaimed
 * only when the last hold goes; a stick pulled while its table is being
 * read is reclaimed too; a plug after a pull takes the number back; and
 * two units of one device are two disks.
 */
extern int printf(const char *fmt, ...);
extern void *memset(void *p, int c, unsigned long n);

#include "../../rach2/sys/block/block.h"
#include "../../rach2/sys/block/disk.h"
#include "../../rach2/sys/proc/wait_queue.h"
#include "../../rach2/sys/block/diskinfo.h"
#include "../../rach2/sys/block/usbdisk.h"
#include "../../rach2/sys/usb/msc.h"

static int fails;

#define CHECK(what, cond)                 \
    do {                                  \
        if (!(cond)) {                    \
            printf("  FAIL: %s\n", what); \
            fails++;                      \
        }                                 \
    } while (0)

#define SECTOR         512u
#define MEDIUM_SECTORS 8192u

/* ---- the class driver: one device with up to two units ---- */

static unsigned char medium[MSC_MAX_LUNS][MEDIUM_SECTORS * SECTOR];
static block_device_t unit[MSC_MAX_DEVICES][MSC_MAX_LUNS];
static int unit_present[MSC_MAX_DEVICES][MSC_MAX_LUNS];
static int releases;

block_device_t *msc_lun_device(int index, int lun)
{
    if (index < 0 || index >= MSC_MAX_DEVICES || lun < 0 || lun >= MSC_MAX_LUNS ||
        !unit_present[index][lun]) {
        return 0;
    }
    return &unit[index][lun];
}

int msc_lun_ident(int index, int lun, struct msc_ident *out)
{
    if (!msc_lun_device(index, lun)) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    out->vendor[0] = 'Q';
    out->product[0] = 'S';
    out->blocks = MEDIUM_SECTORS;
    out->block_size = SECTOR;
    return 0;
}

void msc_release(int index)
{
    int lun;

    releases++;
    for (lun = 0; lun < MSC_MAX_LUNS; lun++) {
        unit_present[index][lun] = 0;
    }
}

/* ---- the request layer: one request in flight, answered next pass ---- */

static struct block_request the_request;
static int request_live;
static uint64_t request_lba;
static uint32_t request_count;
static int request_lun;
static int requests_started;
static int device_holds;

struct block_request *block_request_begin(block_device_t *device, uint64_t lba, uint32_t count,
                                          uint32_t *generation)
{
    int i, lun;

    if (request_live) {
        return 0;
    }
    for (i = 0; i < MSC_MAX_DEVICES; i++) {
        for (lun = 0; lun < MSC_MAX_LUNS; lun++) {
            if (device == &unit[i][lun]) {
                request_lun = lun;
            }
        }
    }
    request_live = 1;
    request_lba = lba;
    request_count = count;
    requests_started++;
    *generation = (uint32_t)requests_started;
    return &the_request;
}

int block_request_ended(struct block_request *r, uint32_t generation, void *buffer, int *status)
{
    unsigned char *out = (unsigned char *)buffer;
    uint32_t i;

    (void)r;
    (void)generation;
    if (!request_live) {
        return 1;
    }
    request_live = 0;
    if (!unit_present[0][request_lun] || request_lba + request_count > MEDIUM_SECTORS) {
        *status = -5;
        return 1;
    }
    for (i = 0; i < request_count * SECTOR; i++) {
        out[i] = medium[request_lun][request_lba * SECTOR + i];
    }
    *status = 0;
    return 1;
}

int block_request(block_device_t *device, int write, uint64_t lba, uint32_t count, void *buffer)
{
    (void)device;
    (void)write;
    (void)lba;
    (void)count;
    (void)buffer;
    return -5;
}

/* The disk layer's scan takes a lock and drops a moved view's cached
   blocks; on the host one scan runs at a time and no cache exists. */
int bcache_invalidate(block_device_t *dev)
{
    (void)dev;
    return 0;
}

int wq_wait_event_nointr(wait_queue_t *wq, wq_cond_fn cond, void *arg)
{
    (void)wq;
    (void)cond;
    (void)arg;
    return 0;
}

void wq_wake_all(wait_queue_t *wq)
{
    (void)wq;
}

int block_device_holds(block_device_t *device)
{
    (void)device;
    return device_holds;
}

/* ---- the block layer's views and physical identity ---- */

static int view_read(block_device_t *device, uint64_t lba, uint32_t count, void *buffer)
{
    (void)device;
    (void)lba;
    (void)count;
    (void)buffer;
    return -5;
}

void block_partition_init(block_partition_t *partition, block_device_t *backing, uint64_t start_lba,
                          uint64_t sector_count)
{
    partition->backing = backing;
    partition->start_lba = start_lba;
    partition->device.sector_size = SECTOR;
    partition->device.sector_count = sector_count;
    partition->device.read = view_read;
    partition->device.write = 0;
    partition->device.submit = 0;
    partition->device.flush = 0;
    partition->device.flags = 0;
    partition->device.drvdata = partition;
}

block_device_t *block_lag_partition_backing(block_device_t *backing, uint64_t start_lba)
{
    (void)start_lba;
    return backing;
}

block_device_t *block_physical(block_device_t *dev)
{
    return dev;
}

/* ---- devfs, the caches and the mounts as counters ---- */

static int nodes_made, parts_named, published;
static int mount_holds, cache_holds, gone_told;

void devfs_disk_nodes(struct disk *d)
{
    (void)d;
    nodes_made++;
}

void devfs_disk_part_nodes(struct disk *d)
{
    (void)d;
    parts_named++;
}

void devfs_disk_publish(struct disk *d)
{
    d->whole.present = 1;
    published++;
}

int devfs_disk_claimed(const struct disk *d)
{
    (void)d;
    return mount_holds != 0;
}

void bcache_device_gone(block_device_t *dev)
{
    (void)dev;
    gone_told++;
}

void loop_device_gone(block_device_t *dev)
{
    (void)dev;
}

int bcache_device_holds(block_device_t *dev)
{
    (void)dev;
    return cache_holds;
}

int vfs_device_holds(block_device_t *dev)
{
    (void)dev;
    return mount_holds;
}

/* ---- the rest of the kernel ---- */

static int in_syscall;

int proc_in_syscall(void)
{
    return in_syscall;
}

uint32_t kirq_save(void)
{
    return 0;
}

void kirq_restore(uint32_t flags)
{
    (void)flags;
}

void klog(int level, const char *subsystem, const char *format, ...)
{
    (void)level;
    (void)subsystem;
    (void)format;
}

void diskinfo_ata_string(char *out, int chars, const volatile uint16_t *id, int first_word)
{
    (void)chars;
    (void)id;
    (void)first_word;
    out[0] = '\0';
}

void diskinfo_record_identify(const char *driver, int port, const volatile uint16_t *id,
                              uint64_t sectors)
{
    (void)driver;
    (void)port;
    (void)id;
    (void)sectors;
}

int diskinfo_u64_dec(char *out, int cap, uint64_t v)
{
    int n = 0, i;
    char tmp[24];

    do {
        tmp[n++] = (char)('0' + (int)(v % 10));
        v /= 10;
    } while (v > 0);
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

#include "../../rach2/sys/block/usbdisk.c"

/* ---- the media ---- */

static void wr32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16);
    p[3] = (unsigned char)(v >> 24);
}

static void wr64(unsigned char *p, uint64_t v)
{
    wr32(p, (uint32_t)v);
    wr32(p + 4, (uint32_t)(v >> 32));
}

/** An MBR with two partitions on unit `lun`. */
static void mbr_medium(int lun)
{
    unsigned char *s = medium[lun];

    memset(s, 0, sizeof(medium[lun]));
    s[446 + 4] = 0x0C;
    wr32(s + 446 + 8, 64);
    wr32(s + 446 + 12, 2048);
    s[462 + 4] = 0x07;
    wr32(s + 462 + 8, 2112);
    wr32(s + 462 + 12, 4096);
    s[510] = 0x55;
    s[511] = 0xAA;
}

/** A GPT with six entries on unit `lun`, the primary and its backup. */
static void gpt_medium(int lun)
{
    unsigned char *s = medium[lun];
    unsigned char *array = s + 2 * SECTOR;
    unsigned char *h = s + SECTOR;
    uint64_t backup_array = MEDIUM_SECTORS - 1 - 32;
    unsigned char *bh = s + (MEDIUM_SECTORS - 1) * SECTOR;
    int i, k;

    memset(s, 0, sizeof(medium[lun]));
    s[446 + 4] = 0xEE;
    wr32(s + 446 + 8, 1);
    wr32(s + 446 + 12, MEDIUM_SECTORS - 1);
    s[510] = 0x55;
    s[511] = 0xAA;
    for (i = 0; i < 6; i++) {
        unsigned char *e = array + i * 128;

        for (k = 0; k < 16; k++) {
            e[k] = (unsigned char)(0xA0 + k);
            e[16 + k] = (unsigned char)(i * 16 + k + 1);
        }
        wr64(e + 32, 64 + (uint64_t)i * 128);
        wr64(e + 40, 64 + (uint64_t)i * 128 + 100);
        e[56] = (unsigned char)('A' + i);
    }
    for (k = 0; k < 2; k++) {
        unsigned char *hdr = k == 0 ? h : bh;

        memset(hdr, 0, SECTOR);
        hdr[0] = 'E';
        hdr[1] = 'F';
        hdr[2] = 'I';
        hdr[3] = ' ';
        hdr[4] = 'P';
        hdr[5] = 'A';
        hdr[6] = 'R';
        hdr[7] = 'T';
        wr32(hdr + 8, 0x00010000u);
        wr32(hdr + 12, 92);
        wr64(hdr + 24, k == 0 ? 1 : MEDIUM_SECTORS - 1);
        wr64(hdr + 32, k == 0 ? MEDIUM_SECTORS - 1 : 1);
        wr64(hdr + 40, 34);
        wr64(hdr + 48, MEDIUM_SECTORS - 34);
        wr64(hdr + 72, k == 0 ? 2 : backup_array);
        wr32(hdr + 80, 128);
        wr32(hdr + 84, 128);
        wr32(hdr + 88, gpt_crc32(array, 128 * 128));
        wr32(hdr + 16, gpt_crc32(hdr, 92));
    }
    for (i = 0; i < 128 * 128; i++) {
        s[backup_array * SECTOR + i] = array[i];
    }
}

/** Run the pump until nothing is pending or `limit` passes have run. */
static int scanning(void)
{
    int i;

    for (i = 0; i < USBDISK_MAX; i++) {
        if (sticks[i].state == STICK_SCANNING) {
            return 1;
        }
    }
    return 0;
}

static int passes(int limit)
{
    int n = 0;

    while (n < limit) {
        usbdisk_step();
        n++;
        if (scan.n < 0 && !pending && !scanning()) {
            break;
        }
    }
    return n;
}

static void plug(int slot, int lun)
{
    unit_present[slot][lun] = 1;
    unit[slot][lun].sector_size = SECTOR;
    unit[slot][lun].sector_count = MEDIUM_SECTORS;
    unit[slot][lun].read = 0;
    unit[slot][lun].write = 0;
    unit[slot][lun].submit = 0;
    unit[slot][lun].flags = 0;
    usbdisk_arrived(slot, lun);
}

int main(void)
{
    struct disk *d;
    int before, i;

    printf("usbdisk_hosttest: the stick as a disk of the registry\n");

    /* An MBR stick: registered, its table read through the pump. */
    mbr_medium(0);
    plug(0, 0);
    CHECK("an arrival is pending", pending);
    passes(40);
    d = disk_at(0);
    CHECK("the stick is disk 0", d != 0 && d->number == 0 && d->present);
    CHECK("as a removable usb disk", d && d->removable && d->transport[0] == 'u');
    CHECK("its table was read", d && d->table.kind == PART_MBR && d->table.count == 2);
    CHECK("in one request, sector 0", requests_started == 1);
    CHECK("its views are published", d && d->views[0].present && d->views[1].present);
    CHECK("and its whole window", d && d->whole.present);
    CHECK("its nodes were made and named", nodes_made == 1 && parts_named >= 1);
    CHECK("the model is the inquiry strings", d && d->model[0] == 'Q' && d->model[2] == 'S');
    CHECK("and nothing is pending", !pending && scan.n < 0);
    CHECK("one record is taken", usbdisk_free() == USBDISK_MAX - 1);

    /* Pulled under a mount: closed at once, reclaimed when the hold goes. */
    mount_holds = 1;
    usbdisk_left(0);
    passes(4);
    CHECK("a pulled stick's nodes are hidden", d && !d->present);
    CHECK("the caches were told", gone_told >= 1);
    CHECK("but it is not reclaimed while a mount holds it",
          releases == 0 && disk_at(0) == d && usbdisk_holds(0) >= 1);
    mount_holds = 0;
    passes(4);
    CHECK("it is reclaimed when the last hold goes",
          releases == 1 && usbdisk_free() == USBDISK_MAX);
    CHECK("and its number is free", disk_count() == 1 && disk_at(0) == 0);

    /* A GPT stick plugged after: the number comes back, the table takes
       several requests. */
    gpt_medium(0);
    before = requests_started;
    plug(0, 0);
    passes(60);
    d = disk_at(0);
    CHECK("the next stick takes number 0 again", d != 0 && d->number == 0);
    CHECK("its GUID table was read", d && d->table.kind == PART_GPT && d->table.count == 6);
    CHECK("across several requests", requests_started - before >= 5);
    CHECK("the fourth entry is named", d && d->table.parts[3].label[0] == 'D');
    CHECK("and no node was made again", nodes_made == 1);

    /* Ten plugs and pulls of a stick nobody holds. */
    usbdisk_left(0);
    passes(4);
    for (i = 0; i < 10; i++) {
        mbr_medium(0);
        plug(0, 0);
        passes(40);
        usbdisk_left(0);
        passes(4);
    }
    CHECK("ten plugs and pulls leave every record free", usbdisk_free() == USBDISK_MAX);
    CHECK("and each was reclaimed", releases == 12 && usbdisk_teardowns() == 12);
    CHECK("and one registry record served them all", disk_count() == 1);

    /* Pulled while the table is being read: reclaimed, nothing published. */
    published = 0;
    mbr_medium(0);
    plug(0, 0);
    usbdisk_step();
    usbdisk_step();
    CHECK("the scan is in progress", scan.n == 0 && request_live);
    usbdisk_left(0);
    passes(8);
    CHECK("a stick pulled mid-scan is reclaimed", releases == 13 && usbdisk_free() == USBDISK_MAX);
    CHECK("and nothing was published for it", published == 0);

    /* Two units of one device are two disks. */
    mbr_medium(0);
    mbr_medium(1);
    plug(0, 0);
    plug(0, 1);
    passes(80);
    CHECK("two units are two disks", disk_at(0) != 0 && disk_at(1) != 0 &&
                                         disk_at(0)->location == 0 && disk_at(1)->location == 1);
    CHECK("each with its table", disk_at(0)->table.count == 2 && disk_at(1)->table.count == 2);
    usbdisk_left(0);
    passes(8);
    CHECK("the device's pull takes both",
          usbdisk_free() == USBDISK_MAX && disk_at(0) == 0 && disk_at(1) == 0);

    /* The pump's settle waits for a syscall to end; the entry's does not. */
    mbr_medium(0);
    plug(0, 0);
    in_syscall = 1;
    usbdisk_step();
    CHECK("the pump leaves a syscall alone", pending && disk_at(0) == 0);
    usbdisk_service();
    CHECK("the syscall entry registers the stick", disk_at(0) != 0);
    in_syscall = 0;
    passes(40);
    CHECK("and the pump reads its table", disk_at(0)->table.count == 2);

    if (fails == 0) {
        printf("usbdisk_hosttest: PASS\n");
        return 0;
    }
    printf("usbdisk_hosttest: %d failure(s)\n", fails);
    return 1;
}
