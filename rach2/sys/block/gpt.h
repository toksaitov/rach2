/*
 * gpt.h: the GUID partition table's header, its entries and its CRC32.
 *
 * A protective MBR, whose first entry has type 0xEE, says the table is here;
 * the header is at sector 1, its backup at the disk's last sector, and the
 * entry array wherever the header says. The header and the whole array are
 * each covered by a CRC32, and both are checked before a field is used, so a
 * table with a damaged entry anywhere in its array is not taken for a sound
 * one.
 *
 * The parsing is pure, as `mbr.c`'s is, so the host test and the fuzz harness
 * drive it with tables no disk would have; reading the sectors is the
 * caller's (`partition.c`).
 */

#ifndef _GPT_H_
#define _GPT_H_

#include "../lib/kint.h"

/** How many partitions `gpt_parse_entries` takes. */
#define GPT_PART_COUNT 4

/** The most entries a header may give, and the most bytes of array this
 * kernel reads. */
#define GPT_MAX_ENTRIES     128
#define GPT_MAX_ARRAY_BYTES 16384u

/** One partition `gpt_parse_entries` took: the sector range it covers. */
struct gpt_part {
    uint64_t start_lba;
    uint64_t sector_count;
};

/** Whether a sector is a protective MBR: signed, with type 0xEE in its first
 * entry, whatever the other three entries are. */
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

/** Fields of a header that `gpt_header_ok` accepted: where the entry array
 * is, how many entries it has and how large each is, the array's CRC32, the
 * other header's sector, and the usable range. */
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
 * the header gives, its last sector below 2^32, and no overlap with an entry
 * already taken; an entry that fails is passed over with the rest still
 * read, so one bad entry does not cost the table its partitions. Returns how
 * many were taken, and in `refused`, when it is not null, how many used
 * entries were not: the ones that failed a check, and those after the
 * fourth taken.
 *
 * The kernel reads tables with `part_scan`; the host test and the GPT fuzz
 * harness call this.
 */
int gpt_parse_entries(const uint8_t *array, uint32_t count, uint32_t entry_size,
                      uint64_t first_usable, uint64_t last_usable,
                      struct gpt_part parts[GPT_PART_COUNT], int *refused);

/** The CRC32 of GUID tables and zip files, over `length` bytes. */
uint32_t gpt_crc32(const uint8_t *data, uint32_t length);

#endif /* _GPT_H_ */
