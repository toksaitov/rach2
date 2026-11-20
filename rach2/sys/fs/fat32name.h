#ifndef _FAT32NAME_H_
#define _FAT32NAME_H_

#include "../lib/kint.h"
#include "fatcommon.h"

/*
 * fat32name: the names a FAT32 volume stores, apart from the driver so the
 * host test reads them alone. The check and conversion of a name are shared
 * by the FAT32 and exFAT drivers and are in fatcommon.h; what is here is
 * FAT32's own.
 *
 * A name that fits 8.3 with each of its two parts all upper or all lower
 * case is stored as a short entry alone, the lower-case parts recorded in
 * the case bits, which is the Windows NT rule and what mtools writes. Any
 * other name is stored in long entries in front of a short entry whose name
 * is an alias: the first six characters of the base, upper-cased and
 * encoded in code page 437, with `~` and a number, and the first three of
 * the last extension; the number is the smallest that no entry of the
 * directory uses, on the raw eleven bytes. The rule is this driver's own
 * and is not claimed to match Windows.
 */

/**
 * Whether `name` is stored as a short entry alone.
 *
 * `name` has passed `fat_name_units`. On 1, `name11` has the eleven bytes
 * of the short name, upper-cased and padded with spaces, a leading 0xE5
 * stored as 0x05, and `*lower` has the case bits of byte 12 of the entry
 * (0x08 for a lower-case base, 0x10 for a lower-case extension). On 0 the
 * name needs long entries and `name11` is undefined.
 */
int fat32_name_short(const char *name, uint8_t *name11, uint8_t *lower);

/**
 * Fill `name11` with the alias of the long name of `count` UTF-16 `units`,
 * numbered 1.
 *
 * The base is up to six units before the last dot and the extension up to
 * three after it. Dots and spaces are dropped; a unit that code page 437
 * holds is stored as its byte, in upper case by the code page's own pairs;
 * a unit outside the short set or the code page, each half of a surrogate
 * pair among them, becomes `_`; an empty base becomes `_`. This is the
 * character mapping of Linux's vfat with codepage=437.
 */
void fat32_alias(const uint16_t *units, uint32_t count, uint8_t *name11);

/**
 * Renumber the alias `name11` to `n`, 1 to 999999, in place: the base is
 * cut to leave room in its eight bytes for `~` and the digits.
 */
void fat32_alias_number(uint8_t *name11, uint32_t n);

/** The most bytes `fat32_short_utf8` writes, the NUL included: eleven
 * characters of up to three bytes and the dot. */
#define FAT32_SHORT_UTF8 36

/**
 * Write to `out`, of `FAT32_SHORT_UTF8` bytes, the short name `name11` as
 * Linux's vfat shows it with codepage=437 and iocharset=utf8.
 *
 * Each part ends at its last byte that is not a space or before a 0 byte,
 * and the dot comes only before an extension. A leading 0x05 reads as
 * 0xE5. Each byte is decoded from code page 437 to UTF-8, a byte 0 as `?`,
 * and a part whose bit in `case_bits` is set (0x08 the base, 0x10 the
 * extension) is first lower-cased by the code page's own pairs.
 */
void fat32_short_utf8(const uint8_t *name11, uint8_t case_bits, char *out);

/** The checksum of a short name that its long entries record. */
uint8_t fat32_short_sum(const uint8_t *name11);

/** The number of long entries a name of `count` UTF-16 units takes. */
uint32_t fat32_long_entries(uint32_t count);

/**
 * Fill the 32 bytes of `raw` with long entry `ord` of the name of `count`
 * UTF-16 units at `units`.
 *
 * `ord` runs from 1 to `entries`, and entry `entries`, the first on the
 * disk, has the last-entry flag 0x40. `sum` is `fat32_short_sum` of the
 * short entry that follows. The units after the name are a 0 and then
 * 0xFFFF.
 */
void fat32_long_entry(uint8_t *raw, uint32_t ord, uint32_t entries, uint8_t sum,
                      const uint16_t *units, uint32_t count);

#endif /* _FAT32NAME_H_ */
