#ifndef GPTDISK_H
#define GPTDISK_H

#include <stdint.h>

/*
 * gptdisk: a disk's identity and its GUID partition table, read and written
 * from user space through the whole-disk node.
 *
 * A disk this system installs is named by its identity, `MBRID=ID` or
 * `GPTID=GUID`, and its partitions by their entry numbers. The kernel reads
 * the same tables (`rach2/sys/block/partition.c`); what is here is what a
 * program needs that the kernel does not do: the identity as text, and a
 * table rewritten with its backup in the disk's last sectors, after an image
 * smaller than the disk was written to it or a partition was grown.
 *
 * The disk's size is the kernel's, from its `disk` line in `/proc/disk`,
 * the number the installer and `growroot` choose a disk by; a write checks
 * that the descriptor it is given reaches the disk's last sector.
 */

#define GPTDISK_SECTOR    512u
#define GPTDISK_ARRAY_MAX 16384u /* 128 entries of 128 bytes */

struct gptdisk {
    uint64_t sectors;                       /* the disk's size */
    unsigned char mbr[GPTDISK_SECTOR];      /* sector 0, the protective MBR */
    unsigned char header[GPTDISK_SECTOR];   /* the header that checked */
    unsigned char array[GPTDISK_ARRAY_MAX]; /* its entry array */
    uint32_t entries;                       /* entries in the array */
    uint32_t entry_size;                    /* bytes in each */
    int from_backup;                        /* the primary failed to check */
};

/** The number N of `/dev/diskN`, or -1 when `path` is not such a name. */
int gptdisk_number(const char *path);

/** The size of disk N in sectors from `/proc/disk`; 0, or -1 when absent. */
int gptdisk_sectors(int number, uint64_t *sectors);

/**
 * Read the GUID table of the disk open on `fd`, of `sectors` sectors, into
 * `g`: the primary header and its array when both checksums agree, else the
 * backup at the disk's last sector. Returns 1 for a table read, 0 when
 * sector 0 is not a protective MBR, and -1 when a sector does not read or
 * neither header checks.
 */
int gptdisk_read(int fd, uint64_t sectors, struct gptdisk *g);

/** The entry numbered `number`, counted from 1, or NULL past the array. */
unsigned char *gptdisk_entry(struct gptdisk *g, int number);

/** Whether an entry is in use: its type GUID is not all zeros. */
int gptdisk_used(const unsigned char *entry);

/** A little-endian 64-bit field, an entry's first or last sector. */
uint64_t gptdisk_le64(const unsigned char *p);

/** Store `v` in a little-endian 64-bit field. */
void gptdisk_put64(unsigned char *p, uint64_t v);

/**
 * Write the table in `g` back to the disk open on `fd`: the backup array and
 * header in the disk's last sectors and the primary array and header after
 * sector 0, each with its CRC32, then the protective MBR's entry over the
 * whole disk. The last usable sector becomes the one before the backup
 * array. The order leaves one copy that checks after any prefix of the
 * writes. A table read from its primary has the backup written first: the
 * old primary checks until the new backup is whole, and the new backup while
 * the primary is rewritten. A table read from its backup, whose primary does
 * not check, has the primary written first, for the same reason the other
 * way round. Returns 0, or -1 with `why` set.
 */
int gptdisk_write(int fd, struct gptdisk *g, const char **why);

/** Whether the backup is already in the disk's last sectors. */
int gptdisk_backup_at_end(const struct gptdisk *g);

/** The disk GUID in the 36-character text form, with its terminator. */
void gptdisk_guid_text(const unsigned char *guid, char out[37]);

/** Parse the 36-character text form into `guid`; 0, or -1 when malformed. */
int gptdisk_guid_parse(const char *text, unsigned char guid[16]);

/** The CRC32 a GUID table carries over its header and its array. */
uint32_t gptdisk_crc32(const unsigned char *data, uint32_t length);

#endif /* GPTDISK_H */
