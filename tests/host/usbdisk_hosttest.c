/*
 * usbdisk_hosttest.c: the stick as a disk, from its table to its last hold.
 *
 * host-deps: rach2/sys/block/gpt.c rach2/sys/block/mbr.c
 *
 * The layer under test reads a partition table through the block interface
 * and publishes what it finds, so the model here is a medium in memory and a
 * `block_request` that copies sectors out of it. Everything else the layer
 * reaches (the device nodes, the buffer cache, the mounts and the class
 * driver) is a stub that records what it was told, because what the rows are
 * about is the judgement, not the plumbing.
 *
 * The rows are the contract: an MBR whose entries are sound, and one with an
 * entry past the medium, one overlapping another and one starting at sector
 * zero, each passed over with the rest still read; a GUID table with its
 * first used entry away from index zero; a GUID table whose entry array
 * fails its checksum in an entry beyond the fourth, which must publish
 * nothing, since a reader that took the first four would take a table that
 * is not sound; a header that fails its own checksum, that names another
 * sector, or that describes an array larger than the bound; a volume with no
 * table at all, published as the whole node alone; a medium that will not
 * give its first sector; the numbering, which is the lowest free one; and
 * the reclaiming, which happens at once for a stick nobody held and waits
 * for the last hold otherwise.
 *
 * No hosted headers: the kernel headers define the fixed-width types, and
 * the few libc calls are declared by hand as the other host tests declare
 * them.
 */

#include "../../rach2/sys/block/block.h"
#include "../../rach2/sys/block/gpt.h"
#include "../../rach2/sys/block/usbdisk.h"
#include "../../rach2/sys/usb/msc.h"

extern int printf(const char *fmt, ...);

#define SECTOR       512
#define DISK_SECTORS 2048u
#define STICKS       2

static unsigned char medium[DISK_SECTORS * SECTOR];
static int unreadable; /* a sector every read of which fails, or -1 */

/* ---- the class driver's side ---- */

static block_device_t stick_dev[STICKS];
static int stick_taken[STICKS];
static int stick_ready_flag[STICKS];
static int released[STICKS];

block_device_t *msc_block_device(int index)
{
    if (index < 0 || index >= STICKS || !stick_taken[index]) {
        return 0;
    }
    return &stick_dev[index];
}

int msc_ready(int index)
{
    return index >= 0 && index < STICKS && stick_ready_flag[index];
}

void msc_release(int index)
{
    if (index >= 0 && index < STICKS) {
        released[index]++;
        stick_taken[index] = 0;
        stick_ready_flag[index] = 0;
    }
}

int msc_ident(int index, struct msc_ident *out)
{
    int i;

    if (index < 0 || index >= STICKS || !stick_taken[index]) {
        return -1;
    }
    for (i = 0; i < (int)sizeof(out->vendor); i++) {
        out->vendor[i] = 0;
    }
    for (i = 0; i < (int)sizeof(out->product); i++) {
        out->product[i] = 0;
    }
    out->vendor[0] = 'Q';
    out->vendor[1] = 0;
    out->product[0] = 'S';
    out->product[1] = 0;
    out->blocks = DISK_SECTORS;
    out->block_size = SECTOR;
    out->write_protect = 0;
    out->closing = 0;
    out->device = index + 1;
    return 0;
}

/* ---- the block interface ----
 *
 * The layer reads without sleeping: it begins a request and asks after it
 * on later passes. The model answers on the pass after the one that began
 * it, so every step of the reading is a separate pass, the way it is on the
 * machine. */

static struct block_request pending_req;
static int pending_live;
static int pending_failed;
static uint32_t generations;

struct block_request *block_request_begin(block_device_t *device, uint64_t lba, uint32_t count,
                                          uint32_t *generation)
{
    (void)device;
    if (pending_live || count == 0 || count > BLOCK_REQ_SECTORS || lba + count > DISK_SECTORS) {
        return 0;
    }
    pending_req.lba = lba;
    pending_req.count = count;
    pending_req.generation = ++generations;
    pending_live = 1;
    pending_failed = 0;
    *generation = pending_req.generation;
    return &pending_req;
}

int block_request_ended(struct block_request *r, uint32_t generation, void *buffer, int *status)
{
    unsigned char *out = (unsigned char *)buffer;
    uint32_t i;

    if (r != &pending_req || generation != pending_req.generation || !pending_live) {
        *status = -1;
        return 1;
    }
    pending_live = 0;
    for (i = 0; i < r->count; i++) {
        uint32_t j;

        if (unreadable >= 0 && (uint64_t)unreadable == r->lba + i) {
            *status = -1;
            return 1;
        }
        for (j = 0; j < SECTOR; j++) {
            out[i * SECTOR + j] = medium[(r->lba + i) * SECTOR + j];
        }
    }
    *status = 0;
    return 1;
}

/* No process is ever inside a syscall here, so every pass may settle. */
int proc_in_syscall(void)
{
    return 0;
}

uint32_t kirq_save(void)
{
    return 0;
}

void kirq_restore(uint32_t flags)
{
    (void)flags;
}

/* A request of the device still in the pool: in flight, or quarantined
   with a page the device may yet write into. */
static block_device_t *request_held_by;

int block_device_holds(block_device_t *device)
{
    return device == request_held_by ? 1 : 0;
}

/* ---- the device nodes ---- */

static struct {
    int published;
    int hidden;
    int withdrawn;
    int parts;
    uint64_t whole;
    uint64_t start[GPT_PART_COUNT];
    uint64_t count[GPT_PART_COUNT];
} node[USBDISK_MAX];

static block_device_t view[USBDISK_MAX][GPT_PART_COUNT + 1];
static int opens[USBDISK_MAX];

void devfs_usb_publish(int n, block_device_t *whole, const struct gpt_part *parts, int count)
{
    int k;

    node[n].published = 1;
    node[n].hidden = 0;
    node[n].withdrawn = 0;
    node[n].parts = count;
    node[n].whole = whole->sector_count;
    for (k = 0; k < GPT_PART_COUNT; k++) {
        node[n].start[k] = k < count ? parts[k].start_lba : 0;
        node[n].count[k] = k < count ? parts[k].sector_count : 0;
    }
}

void devfs_usb_hide(int n)
{
    node[n].hidden = 1;
}

void devfs_usb_withdraw(int n)
{
    node[n].published = 0;
    node[n].withdrawn = 1;
    node[n].parts = 0;
}

block_device_t *devfs_usb_view(int n, int k)
{
    if (!node[n].published || k > node[n].parts) {
        return 0;
    }
    return &view[n][k];
}

int devfs_usb_opens(int n)
{
    return opens[n];
}

/* ---- the cache and the mounts ---- */

static int gone_told[USBDISK_MAX];
static int cache_holds[USBDISK_MAX];

static int view_owner(block_device_t *dev)
{
    int n;
    int k;

    for (n = 0; n < USBDISK_MAX; n++) {
        for (k = 0; k <= GPT_PART_COUNT; k++) {
            if (dev == &view[n][k]) {
                return n;
            }
        }
    }
    return -1;
}

void bcache_device_gone(block_device_t *dev)
{
    int n = view_owner(dev);

    if (n >= 0) {
        gone_told[n]++;
    }
}

int bcache_device_holds(block_device_t *dev)
{
    int n = view_owner(dev);

    if (n >= 0 && dev == &view[n][0]) {
        return cache_holds[n];
    }
    return 0;
}

int vfs_device_holds(block_device_t *dev)
{
    (void)dev;
    return 0;
}

/* ---- the log ---- */

void klog(int level, const char *subsystem, const char *format, ...)
{
    (void)level;
    (void)subsystem;
    (void)format;
}

#include "../../rach2/sys/block/usbdisk.c"

/* ---- the rows ---- */

static int failures;

static void check(const char *what, int ok)
{
    printf("  %s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) {
        failures++;
    }
}

#define CHECK(what, cond) check(what, (cond) ? 1 : 0)

static void put32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)(v & 0xFFu);
    p[1] = (unsigned char)((v >> 8) & 0xFFu);
    p[2] = (unsigned char)((v >> 16) & 0xFFu);
    p[3] = (unsigned char)((v >> 24) & 0xFFu);
}

static void put64(unsigned char *p, uint64_t v)
{
    put32(p, (uint32_t)(v & 0xFFFFFFFFu));
    put32(p + 4, (uint32_t)(v >> 32));
}

static void wipe(void)
{
    uint32_t i;

    for (i = 0; i < sizeof(medium); i++) {
        medium[i] = 0;
    }
    unreadable = -1;
}

/** Put one MBR entry in place: the type, the first sector and the length. */
static void mbr_entry(int index, unsigned char type, uint32_t first, uint32_t count)
{
    unsigned char *e = medium + 446 + index * 16;

    e[4] = type;
    put32(e + 8, first);
    put32(e + 12, count);
}

static void mbr_signature(void)
{
    medium[510] = 0x55;
    medium[511] = 0xAA;
}

/* The GUID table this test writes: a header at sector 1 and an array of
 * `GPT_MAX_ENTRIES` entries of 128 bytes at sector 2, which is what every
 * tool writes and what leaves room for a backup at the end. */
#define ARRAY_LBA    2u
#define ENTRIES      128u
#define ENTRY_SIZE   128u
#define FIRST_USABLE 34u
#define LAST_USABLE  2014u

static unsigned char *entry_at(uint32_t i)
{
    return medium + (ARRAY_LBA * SECTOR) + i * ENTRY_SIZE;
}

/** Mark entry `i` used, covering `[first, first + count)`. */
static void gpt_entry(uint32_t i, uint64_t first, uint64_t count)
{
    unsigned char *e = entry_at(i);
    int j;

    for (j = 0; j < 16; j++) {
        e[j] = (unsigned char)(0xA0 + j); /* a type GUID that is not all zeros */
    }
    put64(e + 32, first);
    put64(e + 40, first + count - 1u);
}

/**
 * Write the header at sector 1 over whatever the array now holds.
 *
 * The array checksum is taken here, so a row that damages the array after
 * this call has a table whose header is sound and whose array is not, which
 * is exactly the case a reader that took four entries at a time would miss.
 */
static void gpt_header(void)
{
    unsigned char *h = medium + SECTOR;
    int i;

    for (i = 0; i < SECTOR; i++) {
        h[i] = 0;
    }
    h[0] = 'E';
    h[1] = 'F';
    h[2] = 'I';
    h[3] = ' ';
    h[4] = 'P';
    h[5] = 'A';
    h[6] = 'R';
    h[7] = 'T';
    put32(h + 8, 0x00010000u);
    put32(h + 12, 92u);
    put64(h + 24, 1u);                /* this header's own sector */
    put64(h + 32, DISK_SECTORS - 1u); /* the backup's */
    put64(h + 40, FIRST_USABLE);
    put64(h + 48, LAST_USABLE);
    put64(h + 72, ARRAY_LBA);
    put32(h + 80, ENTRIES);
    put32(h + 84, ENTRY_SIZE);
    put32(h + 88, gpt_crc32(entry_at(0), ENTRIES * ENTRY_SIZE));
    put32(h + 16, gpt_crc32(h, 92u));
}

/** The protective MBR a GUID table sits behind. */
static void gpt_protective_mbr(void)
{
    mbr_entry(0, 0xEE, 1u, DISK_SECTORS - 1u);
    mbr_signature();
}

/** An exFAT boot sector: a volume with no partition table at all. */
static void exfat_boot_sector(void)
{
    medium[0] = 0xEB;
    medium[1] = 0x76;
    medium[2] = 0x90;
    medium[3] = 'E';
    medium[4] = 'X';
    medium[5] = 'F';
    medium[6] = 'A';
    medium[7] = 'T';
    mbr_signature();
}

/* Put every stick and every number back, so a row starts where the one
 * before it did. */
static void model_reset(void)
{
    int i;

    for (i = 0; i < STICKS; i++) {
        stick_taken[i] = 1;
        stick_ready_flag[i] = 1;
        released[i] = 0;
        stick_dev[i].sector_size = SECTOR;
        stick_dev[i].sector_count = DISK_SECTORS;
        stick_dev[i].flags = 0;
        stick_dev[i].deadline_ticks = 0;
        stick_dev[i].read = 0;
        stick_dev[i].write = 0;
        stick_dev[i].submit = 0;
        stick_dev[i].flush = 0;
        stick_dev[i].drvdata = 0;
    }
    for (i = 0; i < USBDISK_MAX; i++) {
        disks[i].state = DISK_NONE;
        disks[i].marked = 0;
        disks[i].request = 0;
        node[i].published = 0;
        node[i].hidden = 0;
        node[i].withdrawn = 0;
        node[i].parts = 0;
        opens[i] = 0;
        cache_holds[i] = 0;
        gone_told[i] = 0;
    }
    request_held_by = 0;
    pending = 0;
    reading = -1;
    pending_live = 0;
    wipe();
}

/** Run the pump's passes until the layer has nothing left to do. */
static void passes(void)
{
    int i;

    for (i = 0; i < 64; i++) {
        usbdisk_step();
    }
}

/** Bring the stick in slot `slot` up and let the layer read its table. */
static void arrive(int slot)
{
    usbdisk_arrived(slot);
    passes();
}

int main(void)
{
    printf("usbdisk_hosttest: the stick as a disk\n");

    /* An MBR with two sound entries. */
    model_reset();
    mbr_entry(0, 0x0C, 1024u, 512u);
    mbr_entry(1, 0x07, 1536u, 512u);
    mbr_signature();
    arrive(0);
    CHECK("an MBR stick is published", node[0].published);
    CHECK("the whole medium is the first node", node[0].whole == DISK_SECTORS);
    CHECK("both entries become partitions", node[0].parts == 2);
    CHECK("the first covers what it said", node[0].start[0] == 1024u && node[0].count[0] == 512u);
    CHECK("and so does the second", node[0].start[1] == 1536u && node[0].count[1] == 512u);

    /* An entry that runs past the medium is passed over, and the rest are
       still read: a table with one bad entry is not a table with none. */
    model_reset();
    mbr_entry(0, 0x0C, 2000u, 1000u);
    mbr_entry(1, 0x07, 1024u, 512u);
    mbr_signature();
    arrive(0);
    CHECK("an entry past the medium is passed over", node[0].parts == 1);
    CHECK("and the entry after it is still read",
          node[0].start[0] == 1024u && node[0].count[0] == 512u);

    /* An entry overlapping one already taken. */
    model_reset();
    mbr_entry(0, 0x0C, 1024u, 512u);
    mbr_entry(1, 0x07, 1280u, 512u);
    mbr_signature();
    arrive(0);
    CHECK("an overlapping entry is passed over", node[0].parts == 1);

    /* An entry starting at the sector the table itself is in. */
    model_reset();
    mbr_entry(0, 0x0C, 0u, 512u);
    mbr_signature();
    arrive(0);
    CHECK("an entry over the table is passed over", node[0].parts == 0);
    CHECK("and the whole node is published anyway", node[0].published);

    /* A GUID table whose first used entry is not the first entry. */
    model_reset();
    gpt_protective_mbr();
    gpt_entry(3, 40u, 100u);
    gpt_entry(9, 200u, 100u);
    gpt_header();
    arrive(0);
    CHECK("a GUID table is read", node[0].parts == 2);
    CHECK("the first used entry is the first partition",
          node[0].start[0] == 40u && node[0].count[0] == 100u);
    CHECK("wherever in the array it was", node[0].start[1] == 200u && node[0].count[1] == 100u);

    /* The array's checksum covers the whole array. An entry beyond the four
       this kernel publishes is damaged after the header was written, so a
       reader that took four entries at a time would see nothing wrong. */
    model_reset();
    gpt_protective_mbr();
    gpt_entry(0, 40u, 100u);
    gpt_header();
    entry_at(60)[7] ^= 0xFFu;
    arrive(0);
    CHECK("a damaged entry array publishes nothing", node[0].parts == 0);
    CHECK("and the whole node is still published", node[0].published);

    /* A header that fails its own checksum. */
    model_reset();
    gpt_protective_mbr();
    gpt_entry(0, 40u, 100u);
    gpt_header();
    medium[SECTOR + 40] ^= 0xFFu;
    arrive(0);
    CHECK("a damaged header publishes nothing", node[0].parts == 0);

    /* A header that says it is somewhere else. */
    model_reset();
    gpt_protective_mbr();
    gpt_entry(0, 40u, 100u);
    put64(medium + SECTOR + 24, 5u);
    gpt_header();
    put64(medium + SECTOR + 24, 5u);
    put32(medium + SECTOR + 16, 0u);
    put32(medium + SECTOR + 16, gpt_crc32(medium + SECTOR, 92u));
    arrive(0);
    CHECK("a header that names another sector publishes nothing", node[0].parts == 0);

    /* An array larger than the bound. */
    model_reset();
    gpt_protective_mbr();
    gpt_entry(0, 40u, 100u);
    gpt_header();
    put32(medium + SECTOR + 84, 4096u); /* 128 entries of 4 KiB */
    put32(medium + SECTOR + 16, 0u);
    put32(medium + SECTOR + 16, gpt_crc32(medium + SECTOR, 92u));
    arrive(0);
    CHECK("an array past the bound publishes nothing", node[0].parts == 0);

    /* A volume with no table. */
    model_reset();
    exfat_boot_sector();
    arrive(0);
    CHECK("a stick with no table is the whole node alone",
          node[0].published && node[0].parts == 0 && node[0].whole == DISK_SECTORS);

    /* A medium that will not give its first sector. */
    model_reset();
    mbr_entry(0, 0x0C, 1024u, 512u);
    mbr_signature();
    unreadable = 0;
    arrive(0);
    CHECK("an unreadable first sector leaves the whole node",
          node[0].published && node[0].parts == 0);

    /* A GUID table whose array cannot be read. */
    model_reset();
    gpt_protective_mbr();
    gpt_entry(0, 40u, 100u);
    gpt_header();
    unreadable = ARRAY_LBA;
    arrive(0);
    CHECK("an unreadable entry array publishes nothing", node[0].parts == 0);

    /* The numbering: the lowest free one, and it comes back. */
    model_reset();
    mbr_signature();
    arrive(0);
    arrive(1);
    CHECK("two sticks take two numbers", node[0].published && node[1].published);
    CHECK("and no number is left", usbdisk_free() == 0);
    usbdisk_left(0);
    passes();
    CHECK("a stick nobody held is reclaimed at once", !node[0].published && node[0].withdrawn);
    CHECK("its slot goes back to the class driver", released[0] == 1);
    CHECK("the cache is told it has gone", gone_told[0] > 0);
    CHECK("and its number is free again", usbdisk_free() == 1);
    stick_taken[0] = 1;
    stick_ready_flag[0] = 1;
    arrive(0);
    CHECK("the next stick is given the lowest free number", node[0].published);

    /* A stick something still holds is a tombstone until the last hold. */
    model_reset();
    mbr_signature();
    arrive(0);
    opens[0] = 1;
    usbdisk_left(0);
    passes();
    CHECK("a stick with a description open is not reclaimed", released[0] == 0);
    CHECK("its nodes are hidden at once", node[0].hidden);
    CHECK("and its windows are kept", node[0].published);
    CHECK("the cache is told once", gone_told[0] > 0);
    passes();
    CHECK("and it is still not reclaimed while the hold is there", released[0] == 0);
    opens[0] = 0;
    cache_holds[0] = 1;
    passes();
    CHECK("nor while a buffer is held on it", released[0] == 0);
    cache_holds[0] = 0;
    request_held_by = &view[0][0];
    passes();
    CHECK("nor while a request of its is quarantined", released[0] == 0);
    request_held_by = 0;
    usbdisk_service(); /* the syscall entry's half, which does the same */
    CHECK("the last release reclaims it", released[0] == 1 && node[0].withdrawn);
    CHECK("and the cache is not told a second time", gone_told[0] == 1);

    /* A stick pulled before its table was read. */
    model_reset();
    mbr_signature();
    usbdisk_arrived(0);
    stick_ready_flag[0] = 0;
    passes();
    CHECK("a stick that goes before its table is read is not published", !node[0].published);
    CHECK("and is reclaimed", released[0] == 1);

    /* A stick pulled in the middle of its table: the read out for it is
       failed by the class driver and taken back here, and nothing of the
       stick is published. */
    model_reset();
    gpt_protective_mbr();
    gpt_entry(0, 40u, 100u);
    gpt_header();
    usbdisk_arrived(0);
    usbdisk_step(); /* the first sector asked for */
    usbdisk_step(); /* answered; the header asked for */
    CHECK("the table is being read", disks[0].state == DISK_SCANNING);
    usbdisk_left(0);
    stick_ready_flag[0] = 0;
    passes();
    CHECK("a stick that goes while its table is read is not published", !node[0].published);
    CHECK("and is reclaimed once its read has come back", released[0] == 1);
    CHECK("and the next stick can be read", reading == -1);

    printf("usbdisk_hosttest: %s\n", failures == 0 ? "all rows pass" : "rows failed");
    return failures == 0 ? 0 : 1;
}
