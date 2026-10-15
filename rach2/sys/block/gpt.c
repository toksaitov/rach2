/*
 * gpt.c: the GUID partition table's header and entries, checked before any
 * of them is used.
 *
 * Everything here is checked arithmetic on numbers a device supplied. A
 * table says where its own entry array is, how many entries it has and how
 * large each one is, and a reader that multiplied those together without
 * looking would compute an offset the device chose. So every product is
 * bounded first, and the array is read whole and checked against its CRC32
 * before an entry is looked at.
 */

#include "gpt.h"

#include "block.h"

#define GPT_SIGNATURE_0 0x20494645u /* "EFI " */
#define GPT_SIGNATURE_1 0x54524150u /* "PART" */
#define GPT_REVISION    0x00010000u
#define GPT_HEADER_MIN  92u
#define GPT_HEADER_MAX  512u
#define GPT_ENTRY_MIN   128u

#define MBR_TYPE_PROTECTIVE 0xEE

/** The little-endian 32-bit value at `p`. */
static uint32_t read32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/** The little-endian 64-bit value at `p`. */
static uint64_t read64(const uint8_t *p)
{
    return (uint64_t)read32(p) | ((uint64_t)read32(p + 4) << 32);
}

/**
 * The reflected polynomial 0xEDB88320, starting at all ones and inverted at
 * the end. Computed a bit at a time, because an array is at most sixteen
 * kilobytes and a table of remainders would be a kilobyte of data.
 */
uint32_t gpt_crc32(const uint8_t *data, uint32_t length)
{
    uint32_t crc = 0xFFFFFFFFu;
    uint32_t i;

    for (i = 0; i < length; i++) {
        int bit;

        crc ^= (uint32_t)data[i];
        for (bit = 0; bit < 8; bit++) {
            uint32_t mask = (uint32_t)0u - (crc & 1u);

            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

int gpt_protective(const uint8_t *sector)
{
    if (sector[510] != 0x55 || sector[511] != 0xAA) {
        return 0;
    }
    return sector[446 + 4] == MBR_TYPE_PROTECTIVE;
}

uint64_t gpt_array_lba(const uint8_t *header)
{
    return read64(header + 72);
}

uint32_t gpt_entry_count(const uint8_t *header)
{
    return read32(header + 80);
}

uint32_t gpt_entry_size(const uint8_t *header)
{
    return read32(header + 84);
}

uint32_t gpt_array_crc(const uint8_t *header)
{
    return read32(header + 88);
}

uint64_t gpt_alternate_lba(const uint8_t *header)
{
    return read64(header + 32);
}

uint64_t gpt_first_usable(const uint8_t *header)
{
    return read64(header + 40);
}

uint64_t gpt_last_usable(const uint8_t *header)
{
    return read64(header + 48);
}

/**
 * The header must give the sector it was read from, which tells a backup
 * header from a copy of the primary left at the end of the disk. Its CRC32
 * covers the header with the CRC field taken as zero. The entry count times
 * the entry size is bounded before it is computed, since a reader takes an
 * offset from it, and with the disk's size known the whole array, not only
 * its first sector, must be on the medium.
 */
int gpt_header_ok(const uint8_t *header, uint64_t my_lba, uint64_t disk_sectors)
{
    uint32_t size;
    uint32_t stored_crc;
    uint32_t crc;
    uint8_t copy[GPT_HEADER_MAX];
    uint32_t i;
    uint64_t first_usable;
    uint64_t last_usable;
    uint64_t array_lba;
    uint32_t count;
    uint32_t entry_size;

    if (read32(header) != GPT_SIGNATURE_0 || read32(header + 4) != GPT_SIGNATURE_1) {
        return -1;
    }
    if (read32(header + 8) != GPT_REVISION) {
        return -1;
    }
    size = read32(header + 12);
    if (size < GPT_HEADER_MIN || size > GPT_HEADER_MAX) {
        return -1;
    }
    if (read64(header + 24) != my_lba) {
        return -1;
    }

    stored_crc = read32(header + 16);
    for (i = 0; i < size; i++) {
        copy[i] = header[i];
    }
    copy[16] = 0;
    copy[17] = 0;
    copy[18] = 0;
    copy[19] = 0;
    crc = gpt_crc32(copy, size);
    if (crc != stored_crc) {
        return -1;
    }

    first_usable = gpt_first_usable(header);
    last_usable = gpt_last_usable(header);
    array_lba = gpt_array_lba(header);
    count = gpt_entry_count(header);
    entry_size = gpt_entry_size(header);

    if (count == 0 || count > GPT_MAX_ENTRIES) {
        return -1;
    }
    if (entry_size < GPT_ENTRY_MIN || (entry_size & 0x7Fu) != 0) {
        return -1;
    }
    if (entry_size > GPT_MAX_ARRAY_BYTES / count) {
        return -1;
    }
    if (first_usable > last_usable) {
        return -1;
    }
    if (disk_sectors != 0 && last_usable >= disk_sectors) {
        return -1;
    }
    if (array_lba == 0 || (disk_sectors != 0 && array_lba >= disk_sectors)) {
        return -1;
    }
    if (disk_sectors != 0 && (uint64_t)((entry_size * count + BLOCK_SECTOR_SIZE - 1u) /
                                        BLOCK_SECTOR_SIZE) > disk_sectors - array_lba) {
        return -1;
    }
    return 0;
}

int gpt_parse_entries(const uint8_t *array, uint32_t count, uint32_t entry_size,
                      uint64_t first_usable, uint64_t last_usable,
                      struct gpt_part parts[GPT_PART_COUNT], int *refused)
{
    uint32_t i;
    int taken = 0;
    int passed_over = 0;

    for (i = 0; i < GPT_PART_COUNT; i++) {
        parts[i].start_lba = 0;
        parts[i].sector_count = 0;
    }
    for (i = 0; i < count; i++) {
        const uint8_t *entry = array + (uint32_t)i * entry_size;
        uint64_t first;
        uint64_t last;
        int used = 0;
        int j;
        int overlaps = 0;

        for (j = 0; j < 16; j++) {
            if (entry[j] != 0) {
                used = 1;
            }
        }
        if (!used) {
            continue;
        }
        if (taken == GPT_PART_COUNT) {
            passed_over++;
            continue;
        }
        first = read64(entry + 32);
        last = read64(entry + 40);
        if (first > last || first < first_usable || last > last_usable) {
            passed_over++;
            continue;
        }
        for (j = 0; j < taken; j++) {
            uint64_t other_first = parts[j].start_lba;
            uint64_t other_last = other_first + parts[j].sector_count - 1u;

            if (first <= other_last && other_first <= last) {
                overlaps = 1;
            }
        }
        if (overlaps) {
            passed_over++;
            continue;
        }
        if (last > 0xFFFFFFFFu) {
            passed_over++;
            continue;
        }
        parts[taken].start_lba = first;
        parts[taken].sector_count = last - first + 1u;
        taken++;
    }
    if (refused != 0) {
        *refused = passed_over;
    }
    return taken;
}
