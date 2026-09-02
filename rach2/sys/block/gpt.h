#ifndef _GPT_H_
#define _GPT_H_

#include "../lib/kint.h"

/*
 * gpt: the GUID partition table, as every stick formatted on a Mac has and
 * every disk over two terabytes must have.
 *
 * A protective MBR of type 0xEE at entry zero says the table is here rather
 * than there; the header is at sector 1, and the entry array wherever the
 * header says. Both the header and the whole array are covered by a CRC32,
 * and both are checked before a single field is used, because an array read
 * four entries at a time would take a table whose fifth entry is rubbish
 * for a table that is sound.
 *
 * The reader is split the way `mbr.c` is: the parsing is pure, so the host
 * test drives it against tables no disk would carry, and the reading of
 * sectors is the caller's.
 */

/** How many partitions this kernel publishes from a table, MBR or GUID. */
#define GPT_PART_COUNT 4

/** The most a table may describe, and the most of it this kernel reads. */
#define GPT_MAX_ENTRIES     128
#define GPT_MAX_ARRAY_BYTES 16384u

/** One published partition: the half-open sector range it covers. */
struct gpt_part {
    uint64_t start_lba;
    uint64_t sector_count;
};

/** Whether an MBR sector is the protective one a GUID table sits behind. */
int gpt_protective(const uint8_t *sector);

/**
 * Judge a GUID header on its own: its signature, revision, size, the CRC32
 * over it, where it says it is, and what it says about its entry array.
 *
 * `my_lba` is the sector the header was read from, and `disk_sectors` the
 * size of the device, or 0 when it is not known; with it known, the usable
 * range and the whole of the entry array must lie on the device. Returns 0
 * when the header is sound, -1 otherwise.
 */
int gpt_header_ok(const uint8_t *header, uint64_t my_lba, uint64_t disk_sectors);

/** Where the entry array is, how many entries it has, and how large each is. */
uint64_t gpt_array_lba(const uint8_t *header);
uint32_t gpt_entry_count(const uint8_t *header);
uint32_t gpt_entry_size(const uint8_t *header);
uint32_t gpt_array_crc(const uint8_t *header);
uint64_t gpt_alternate_lba(const uint8_t *header);
uint64_t gpt_first_usable(const uint8_t *header);
uint64_t gpt_last_usable(const uint8_t *header);

/**
 * Take the first four used entries out of an array whose CRC32 the caller
 * has already checked.
 *
 * An entry is used when its type GUID is not all zeros. Each is checked to
 * have its first sector at or before its last, both inside the usable range
 * the header gives, and no overlap with an entry already taken; an entry
 * that fails is passed over with the rest still read, because a table with
 * one bad entry is not a table with no partitions. Returns how many were
 * published, and in `refused`, when it is not null, how many used entries
 * were not: the ones that failed a check, and the sound ones beyond the
 * four this kernel publishes.
 */
int gpt_parse_entries(const uint8_t *array, uint32_t count, uint32_t entry_size,
                      uint64_t first_usable, uint64_t last_usable,
                      struct gpt_part parts[GPT_PART_COUNT], int *refused);

/** The CRC32 every GUID table and zip file uses. */
uint32_t gpt_crc32(const uint8_t *data, uint32_t length);

#endif /* _GPT_H_ */
