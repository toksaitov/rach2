#ifndef _PARTITION_H_
#define _PARTITION_H_

#include "../lib/kint.h"
#include "gpt.h"
#include "mbr.h"

/*
 * The partition service: one reader of a disk's table for every disk on
 * every bus.
 *
 * A disk has an MBR, a GPT, or nothing. From an MBR the four primary
 * entries are taken, and the logical partitions of its extended entry are
 * followed through the chain of extended boot records, numbered from 5 in
 * chain order as every other system numbers them. From a GPT every used
 * entry is taken with its type GUID, its own GUID and its name, from the
 * primary header when it and its array check, otherwise from the backup at
 * the end of the disk. A partition's number is its entry's position in the
 * table, counted from 1, so the number is stable across boots whatever
 * else is on the disk.
 *
 * The reader is a callback so the same code runs in the kernel over a block
 * device and in the host test over a table in memory.
 */

/* How many partitions of one disk are kept. A table with more has the
   rest counted in `refused`, and no number is above this, which keeps the
   node minors in their range. */
#define PART_MAX 15

#define PART_LABEL_CHARS 36 /* a GPT name, 36 UTF-16 units */

/* What the first sectors turned out to be. */
#define PART_UNREAD  0 /* sector 0 could not be read */
#define PART_NONE    1 /* no table: no signature, or a signature and no entries */
#define PART_MBR     2
#define PART_GPT     3
#define PART_GPT_BAD 4 /* a protective MBR, and neither header checks */

/* The MBR types that hold an extended partition. */
#define PART_MBR_EXTENDED_CHS   0x05
#define PART_MBR_EXTENDED_LBA   0x0F
#define PART_MBR_EXTENDED_LINUX 0x85

struct part_entry {
    int number; /* 1 upward; MBR logical partitions from 5 */
    uint64_t start_lba;
    uint64_t sector_count;
    uint8_t mbr_type; /* the type byte, MBR only */
    uint8_t type_guid[16];
    uint8_t guid[16];                 /* the partition's own GUID, GPT only */
    char label[PART_LABEL_CHARS + 1]; /* the GPT name as ASCII, '?' elsewhere */
};

/* Where a GUID table's own sectors are, as the header that served says. */
struct part_gpt_meta {
    uint64_t header_lba;    /* the header read: 1, or the disk's last sector */
    uint64_t alternate_lba; /* the other header */
    uint64_t array_lba;     /* the entry array the header covers */
    uint32_t array_sectors;
    uint64_t first_usable; /* the sectors an entry may cover */
    uint64_t last_usable;
};

struct part_table {
    int kind; /* PART_* */
    uint32_t mbr_id;
    uint8_t disk_guid[16];
    int count;
    int refused;     /* entries not taken: past PART_MAX, outside the disk, overlapping */
    int backup_used; /* GPT: the primary was bad and the backup served */
    int volume;      /* no table, and sector 0 is a FAT or exFAT boot sector: one volume */
    /* GPT only; zero otherwise */
    struct part_gpt_meta gpt;
    struct part_entry parts[PART_MAX];
};

/** Read `count` sectors from `lba` into `buf`; 0 on success, else negative. */
typedef int (*part_read_fn)(void *ctx, uint64_t lba, uint32_t count, void *buf);

/* The work area `part_scan` needs: the largest entry array and two sectors. */
#define PART_WORK_BYTES (GPT_MAX_ARRAY_BYTES + 2u * 512u)

/**
 * Read the table of a disk of `disk_sectors` sectors through `read`.
 *
 * Fills `t` and returns 0 when sector 0 was read, whatever it held; -1 when
 * it could not be read, with `kind` `PART_UNREAD`. `work` has
 * `PART_WORK_BYTES` bytes.
 */
int part_scan(struct part_table *t, part_read_fn read, void *ctx, uint64_t disk_sectors,
              uint8_t *work);

/**
 * Give the entries numbered 1 to 4 of the GUID table `t` as the four records
 * of an MBR, for the judgements `mbr.c` makes of a boot disk's table.
 *
 * Entry N goes in record N - 1, with the MBR type that says what its type
 * GUID says: 0xEF for an EFI system partition, 0x0C for basic data, 0x83 for
 * a Linux file system, and 0xDA, data in no file system an MBR names, for
 * any other. A number with no entry, or an entry that reaches past what 32
 * bits of sector address hold, leaves its record empty. Returns 0, or -1
 * with every record empty when `t` is not a GUID table.
 */
int part_gpt_as_mbr(const struct part_table *t, struct mbr_part parts[MBR_PART_COUNT]);

/**
 * Find the BIOS boot partition of the GUID table `t`: the entry of type
 * 21686148-6449-6E6F-744E-656564454649, where GRUB's BIOS core lives on a
 * GPT disk, since the sectors after the MBR are the table's.
 *
 * The table must have exactly one such entry among those taken, and it must
 * lie inside the usable range the serving header gives, below 2^32 sectors,
 * and clear of sector 0, the primary header at sector 1, the serving header,
 * the other header it names, and the entry array it covers. The other copy's
 * array is not read: the specification puts it outside the usable range, and
 * the entry must also be clear of the place every partitioning tool gives it,
 * the same number of sectors beside the other header.
 *
 * Returns 0 with the entry's first sector, its length and its number set, or
 * -1 with nothing set.
 */
int part_bios_boot(const struct part_table *t, uint64_t *first, uint64_t *count, int *number);

/** Write `guid` as the 36-character text form into `out`, terminated. */
void part_guid_text(const uint8_t guid[16], char out[37]);

/** Parse the 36-character text form into `guid`; 0 on success, -1 otherwise. */
int part_guid_parse(const char *text, uint8_t guid[16]);

/** Whether two GUIDs are equal. */
int part_guid_equal(const uint8_t a[16], const uint8_t b[16]);

#endif /* _PARTITION_H_ */
