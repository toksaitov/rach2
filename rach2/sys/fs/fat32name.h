#ifndef _FAT32NAME_H_
#define _FAT32NAME_H_

#include "../lib/kint.h"

/*
 * fat32name: the names a FAT32 volume stores, apart from the driver so the
 * host test reads them alone.
 *
 * A name is checked before anything is written: at most 255 bytes of UTF-8
 * converted to at most 255 UTF-16 units, none of the characters FAT forbids,
 * no trailing dot or space, and neither `.` nor `..`. A name that fits 8.3
 * with each of its two parts all upper or all lower case is stored as a
 * short entry alone, the lower-case parts recorded in the case bits, which
 * is the Windows NT rule and what mtools writes. Any other name is stored in
 * long entries in front of a short entry whose name is an alias: the first
 * six characters of the base that a short name may hold, upper-cased, with
 * `~` and a number, and the first three of the last extension; the number
 * is the smallest that no entry of the directory uses, on the raw eleven
 * bytes. The rule is this driver's own and is not claimed to match Windows.
 */

#define FAT32_NAME_UNITS 255

/** UTF-8 to UTF-16, checked: 0 with `units` and `*count` filled, -EINVAL
 * for a name FAT cannot store, -ENAMETOOLONG past 255 units. */
int fat32_name_units(const char *name, uint16_t *units, uint32_t *count);

/** Whether `name` (already checked) is stored short alone: 1 with `name11`
 * and the case bits filled, 0 when long entries are needed. */
int fat32_name_short(const char *name, uint8_t *name11, uint8_t *lower);

/** The alias base for a long name: the eleven bytes with `~1`. */
void fat32_alias(const char *name, uint8_t *name11);

/** The alias renumbered `n` (1 to 999999) in place. */
void fat32_alias_number(uint8_t *name11, uint32_t n);

/** The checksum of a short name that its long entries record. */
uint8_t fat32_short_sum(const uint8_t *name11);

/** The count of long entries `count` units take. */
uint32_t fat32_long_entries(uint32_t count);

/** Fill long entry `ord` (1 to the count; the highest is written first
 * and carries the last flag) for the `count` units with checksum `sum`. */
void fat32_long_entry(uint8_t *raw, uint32_t ord, uint32_t entries, uint8_t sum,
                      const uint16_t *units, uint32_t count);

#endif /* _FAT32NAME_H_ */
