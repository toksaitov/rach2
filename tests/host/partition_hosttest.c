/*
 * partition_hosttest.c: the partition service over disks in memory.
 *
 * host-deps: rach2/sys/block/partition.c rach2/sys/block/gpt.c
 * host-deps: rach2/sys/block/mbr.c
 *
 * A disk is an array of sectors the reader callback serves, so every table
 * the service must read is built here byte by byte: an MBR with an extended
 * partition and a chain of logical ones, a chain that loops, a GPT with
 * more entries than four, a GPT whose primary is damaged and whose backup
 * serves, one with both damaged, a blank disk, an unreadable one, and more
 * entries than the service keeps. The installed disk's GPT layout is read
 * again as the four records of an MBR, which is how the kernel judges its
 * boot partition.
 */
extern int printf(const char *fmt, ...);
extern int strcmp(const char *a, const char *b);
extern void *memset(void *p, int c, unsigned long n);

#include "../../rach2/sys/block/partition.h"

static int fails;

#define CHECK(what, cond)                 \
    do {                                  \
        if (!(cond)) {                    \
            printf("  FAIL: %s\n", what); \
            fails++;                      \
        }                                 \
    } while (0)

#define SECTOR       512u
#define DISK_SECTORS 4096u
#define ENTRIES      128u
#define ENTRY_SIZE   128u
#define ARRAY_BYTES  (ENTRIES * ENTRY_SIZE)
#define ARRAY_SECS   (ARRAY_BYTES / SECTOR)

static unsigned char disk[DISK_SECTORS * SECTOR];
static unsigned char work[PART_WORK_BYTES];
static int unreadable_lba = -1;
static int reads;

static int read_disk(void *ctx, uint64_t lba, uint32_t count, void *buf)
{
    unsigned char *out = (unsigned char *)buf;
    uint32_t i;

    (void)ctx;
    reads++;
    if (lba + count > DISK_SECTORS) {
        return -1;
    }
    for (i = 0; i < count; i++) {
        if ((int)(lba + i) == unreadable_lba) {
            return -1;
        }
    }
    for (i = 0; i < count * SECTOR; i++) {
        out[i] = disk[(unsigned long)lba * SECTOR + i];
    }
    return 0;
}

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

/** An MBR entry `index` in the sector at `lba`. */
static void mbr_entry(uint64_t lba, int index, unsigned type, uint32_t start, uint32_t count)
{
    unsigned char *e = disk + lba * SECTOR + 446 + index * 16;

    e[4] = (unsigned char)type;
    wr32(e + 8, start);
    wr32(e + 12, count);
    disk[lba * SECTOR + 510] = 0x55;
    disk[lba * SECTOR + 511] = 0xAA;
}

static void blank(void)
{
    memset(disk, 0, sizeof(disk));
    unreadable_lba = -1;
}

/* ---- GPT building ---- */

static void gpt_entry(unsigned index, uint64_t first, uint64_t last, const char *name)
{
    unsigned char *e = disk + 2 * SECTOR + index * ENTRY_SIZE;
    int i;

    for (i = 0; i < 16; i++) {
        e[i] = (unsigned char)(0xA0 + i);
        e[16 + i] = (unsigned char)(index * 16 + i + 1);
    }
    wr64(e + 32, first);
    wr64(e + 40, last);
    for (i = 0; name[i] && i < 36; i++) {
        e[56 + i * 2] = (unsigned char)name[i];
        e[57 + i * 2] = 0;
    }
}

static void gpt_header(uint64_t my_lba, uint64_t alternate, uint64_t array_lba)
{
    unsigned char *h = disk + my_lba * SECTOR;
    int i;

    memset(h, 0, SECTOR);
    h[0] = 'E';
    h[1] = 'F';
    h[2] = 'I';
    h[3] = ' ';
    h[4] = 'P';
    h[5] = 'A';
    h[6] = 'R';
    h[7] = 'T';
    wr32(h + 8, 0x00010000u);
    wr32(h + 12, 92);
    wr64(h + 24, my_lba);
    wr64(h + 32, alternate);
    wr64(h + 40, 34);
    wr64(h + 48, DISK_SECTORS - 34u);
    for (i = 0; i < 16; i++) {
        h[56 + i] = (unsigned char)(0xD0 + i);
    }
    wr64(h + 72, array_lba);
    wr32(h + 80, ENTRIES);
    wr32(h + 84, ENTRY_SIZE);
    wr32(h + 88, gpt_crc32(disk + 2 * SECTOR, ARRAY_BYTES));
    wr32(h + 16, gpt_crc32(h, 92));
}

/** Give entry `index` the type GUID `type`, as the table stores it. */
static void gpt_type(unsigned index, const unsigned char *type)
{
    unsigned char *e = disk + 2 * SECTOR + index * ENTRY_SIZE;
    int i;

    for (i = 0; i < 16; i++) {
        e[i] = type[i];
    }
}

/** A whole GPT disk: protective MBR, the entries given, both headers. */
static void gpt_disk(void)
{
    uint64_t backup_array = DISK_SECTORS - 1 - ARRAY_SECS;
    unsigned long i;

    mbr_entry(0, 0, 0xEE, 1, DISK_SECTORS - 1);
    gpt_header(1, DISK_SECTORS - 1, 2);
    for (i = 0; i < ARRAY_BYTES; i++) {
        disk[backup_array * SECTOR + i] = disk[2 * SECTOR + i];
    }
    gpt_header(DISK_SECTORS - 1, 1, backup_array);
}

int main(void)
{
    static struct part_table t;
    char text[37];
    uint8_t guid[16];
    int i;

    /* ---- nothing ---- */
    blank();
    CHECK("a blank disk reads", part_scan(&t, read_disk, 0, DISK_SECTORS, work) == 0);
    CHECK("and has no table", t.kind == PART_NONE && t.count == 0);
    unreadable_lba = 0;
    CHECK("an unreadable sector 0 is reported",
          part_scan(&t, read_disk, 0, DISK_SECTORS, work) == -1);
    CHECK("as unread", t.kind == PART_UNREAD);
    blank();
    disk[510] = 0x55;
    disk[511] = 0xAA;
    CHECK("a signature with no entries is no table",
          part_scan(&t, read_disk, 0, DISK_SECTORS, work) == 0 && t.kind == PART_NONE);

    /* ---- an MBR with primaries and a chain of logical partitions ---- */
    blank();
    wr32(disk + 440, 0x1acef00du);
    mbr_entry(0, 0, 0x0C, 64, 64);
    mbr_entry(0, 1, 0x83, 128, 64);
    mbr_entry(0, 2, 0x05, 256, 1024); /* the extended partition */
    mbr_entry(0, 3, 0x7F, 2048, 512);
    /* EBR 1 at 256: logical at 256+8 for 120, next EBR at 256+128. */
    mbr_entry(256, 0, 0x83, 8, 120);
    mbr_entry(256, 1, 0x05, 128, 128);
    /* EBR 2 at 384: logical at 384+8 for 120, next EBR at 256+256. */
    mbr_entry(384, 0, 0x0C, 8, 120);
    mbr_entry(384, 1, 0x05, 256, 128);
    /* EBR 3 at 512: the last logical. */
    mbr_entry(512, 0, 0x83, 8, 120);
    CHECK("the MBR disk reads", part_scan(&t, read_disk, 0, DISK_SECTORS, work) == 0);
    CHECK("as an MBR", t.kind == PART_MBR);
    CHECK("with its id", t.mbr_id == 0x1acef00du);
    CHECK("three primaries and three logical partitions", t.count == 6);
    CHECK("nothing refused", t.refused == 0);
    CHECK("primary 1", t.parts[0].number == 1 && t.parts[0].start_lba == 64 &&
                           t.parts[0].sector_count == 64 && t.parts[0].mbr_type == 0x0C);
    CHECK("primary 2", t.parts[1].number == 2 && t.parts[1].start_lba == 128);
    CHECK("primary 4, numbered by its entry",
          t.parts[2].number == 4 && t.parts[2].start_lba == 2048);
    CHECK("logical 5", t.parts[3].number == 5 && t.parts[3].start_lba == 264 &&
                           t.parts[3].sector_count == 120 && t.parts[3].mbr_type == 0x83);
    CHECK("logical 6", t.parts[4].number == 6 && t.parts[4].start_lba == 392);
    CHECK("logical 7", t.parts[5].number == 7 && t.parts[5].start_lba == 520);
    CHECK("an MBR entry has no GUID", t.parts[0].guid[0] == 0 && t.parts[0].label[0] == '\0');

    /* A chain that points back at itself ends. */
    mbr_entry(512, 1, 0x05, 0, 128); /* next at ext_start itself */
    CHECK("a looping chain reads", part_scan(&t, read_disk, 0, DISK_SECTORS, work) == 0);
    CHECK("keeps the partitions before the loop", t.count == 6);
    CHECK("and counts the loop as refused", t.refused == 1);

    /* A logical partition outside its extended partition is refused. */
    mbr_entry(512, 1, 0, 0, 0);
    mbr_entry(512, 0, 0x83, 8, 4000);
    CHECK("a logical partition past the extended one reads",
          part_scan(&t, read_disk, 0, DISK_SECTORS, work) == 0);
    CHECK("and is refused", t.count == 5 && t.refused == 1);

    /* Overlap and disk end on primaries. */
    blank();
    mbr_entry(0, 0, 0x83, 64, 64);
    mbr_entry(0, 1, 0x83, 100, 64);   /* overlaps entry 1 */
    mbr_entry(0, 2, 0x83, 4000, 200); /* past the disk */
    mbr_entry(0, 3, 0x83, 0, 64);     /* starts on the MBR */
    CHECK("bad primaries read", part_scan(&t, read_disk, 0, DISK_SECTORS, work) == 0);
    CHECK("one taken, three refused", t.count == 1 && t.refused == 3);

    /* ---- a GPT with six entries ---- */
    blank();
    for (i = 0; i < 6; i++) {
        char name[8];

        name[0] = 'P';
        name[1] = (char)('0' + i);
        name[2] = '\0';
        gpt_entry((unsigned)i, 64 + (uint64_t)i * 128, 64 + (uint64_t)i * 128 + 100, name);
    }
    gpt_disk();
    CHECK("the GPT disk reads", part_scan(&t, read_disk, 0, DISK_SECTORS, work) == 0);
    CHECK("as a GPT from the primary", t.kind == PART_GPT && t.backup_used == 0);
    CHECK("with six partitions", t.count == 6 && t.refused == 0);
    CHECK("the disk GUID", t.disk_guid[0] == 0xD0 && t.disk_guid[15] == 0xDF);
    CHECK("entry 3's range",
          t.parts[2].number == 3 && t.parts[2].start_lba == 320 && t.parts[2].sector_count == 101);
    CHECK("entry 3's GUID", t.parts[2].guid[0] == 0x21 && t.parts[2].guid[15] == 0x30);
    CHECK("entry 3's label", strcmp(t.parts[2].label, "P2") == 0);
    part_guid_text(t.parts[2].guid, text);
    CHECK("the GUID's text form", strcmp(text, "24232221-2625-2827-292a-2b2c2d2e2f30") == 0);
    CHECK("which parses back",
          part_guid_parse(text, guid) == 0 && part_guid_equal(guid, t.parts[2].guid));
    CHECK("a malformed text is refused",
          part_guid_parse("24232221-2625-2827-292a-2b2c2d2e2f3", guid) == -1);
    CHECK("and a wrong separator",
          part_guid_parse("24232221:2625-2827-292a-2b2c2d2e2f30", guid) == -1);

    /* The primary header damaged: the backup serves. */
    disk[1 * SECTOR + 20] ^= 0xFF;
    CHECK("a damaged primary reads", part_scan(&t, read_disk, 0, DISK_SECTORS, work) == 0);
    CHECK("from the backup", t.kind == PART_GPT && t.backup_used == 1 && t.count == 6);
    /* The primary array damaged: the backup serves too. */
    disk[1 * SECTOR + 20] ^= 0xFF;
    disk[2 * SECTOR + 40] ^= 0xFF;
    CHECK("a damaged primary array reads", part_scan(&t, read_disk, 0, DISK_SECTORS, work) == 0);
    CHECK("from the backup", t.kind == PART_GPT && t.backup_used == 1 && t.count == 6);
    /* Both damaged: a GPT that cannot be read, and no partitions. */
    disk[(DISK_SECTORS - 1) * SECTOR + 20] ^= 0xFF;
    CHECK("both damaged reads", part_scan(&t, read_disk, 0, DISK_SECTORS, work) == 0);
    CHECK("as a bad GPT", t.kind == PART_GPT_BAD && t.count == 0);

    /* The installed disk: the EFI system partition, recovery, the root and
     * the BIOS boot partition in the sectors before the first, as entries
     * 1 to 4, and a fifth the MBR's records have no room for. */
    {
        static const unsigned char esp[16] = {0x28, 0x73, 0x2a, 0xc1, 0x1f, 0xf8, 0xd2, 0x11,
                                              0xba, 0x4b, 0x00, 0xa0, 0xc9, 0x3e, 0xc9, 0x3b};
        static const unsigned char data[16] = {0xa2, 0xa0, 0xd0, 0xeb, 0xe5, 0xb9, 0x33, 0x44,
                                               0x87, 0xc0, 0x68, 0xb6, 0xb7, 0x26, 0x99, 0xc7};
        static const unsigned char linux_fs[16] = {0xaf, 0x3d, 0xc6, 0x0f, 0x83, 0x84, 0x72, 0x47,
                                                   0x8e, 0x79, 0x3d, 0x69, 0xd8, 0x47, 0x7d, 0xe4};
        static const unsigned char bios[16] = {0x48, 0x61, 0x68, 0x21, 0x49, 0x64, 0x6f, 0x6e,
                                               0x74, 0x4e, 0x65, 0x65, 0x64, 0x45, 0x46, 0x49};
        struct mbr_part m[MBR_PART_COUNT];

        blank();
        gpt_entry(0, 2048, 2559, "boot");
        gpt_type(0, esp);
        gpt_entry(1, 2560, 3071, "recovery");
        gpt_type(1, data);
        gpt_entry(2, 3072, 3583, "root");
        gpt_type(2, linux_fs);
        gpt_entry(3, 34, 2047, "BIOS boot");
        gpt_type(3, bios);
        gpt_entry(4, 3584, 3839, "fifth");
        gpt_disk();
        CHECK("the installed layout reads",
              part_scan(&t, read_disk, 0, DISK_SECTORS, work) == 0 && t.count == 5);
        CHECK("as four MBR records", part_gpt_as_mbr(&t, m) == 0);
        CHECK("entry 1, the EFI system partition, as type 0xEF",
              m[0].type == 0xEF && m[0].start_lba == 2048 && m[0].sector_count == 512);
        CHECK("entry 2, basic data, as 0x0C", m[1].type == 0x0C && m[1].start_lba == 2560);
        CHECK("entry 3, a Linux file system, as 0x83",
              m[2].type == 0x83 && m[2].sector_count == 512);
        CHECK("entry 4, BIOS boot, as data in no file system",
              m[3].type == 0xDA && m[3].start_lba == 34 && m[3].sector_count == 2014);
        CHECK("which the boot partition's judgement takes", mbr_bootpart_ok(m, DISK_SECTORS) == 0);
        t.parts[0].start_lba = 0x100000000ull;
        CHECK("an entry past 32 bits of address", part_gpt_as_mbr(&t, m) == 0);
        CHECK("leaves its record empty", m[0].type == 0 && m[0].sector_count == 0);
        t.kind = PART_MBR;
        CHECK("an MBR table is not given this way", part_gpt_as_mbr(&t, m) == -1);
        CHECK("and its records are empty", m[1].type == 0 && m[2].sector_count == 0);
    }

    /* More entries than the service keeps. */
    blank();
    for (i = 0; i < PART_MAX + 3; i++) {
        gpt_entry((unsigned)i, 64 + (uint64_t)i * 64, 64 + (uint64_t)i * 64 + 32, "x");
    }
    gpt_disk();
    CHECK("a GPT with more entries than kept reads",
          part_scan(&t, read_disk, 0, DISK_SECTORS, work) == 0);
    CHECK("keeps the first PART_MAX",
          t.count == PART_MAX && t.parts[PART_MAX - 1].number == PART_MAX);
    CHECK("and counts the rest", t.refused == 3);

    if (fails == 0) {
        printf("partition_hosttest: PASS\n");
        return 0;
    }
    printf("partition_hosttest: %d failure(s)\n", fails);
    return 1;
}
