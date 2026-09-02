#ifndef _FATCOMMON_H_
#define _FATCOMMON_H_

#include "../lib/kint.h"

/*
 * fatcommon: what the FAT32 and exFAT drivers both need and neither owns.
 * The check and conversion of a name, the DOS timestamp bit fields, the
 * first-fit allocation hint, and the dirty-volume walk's bitmap with its
 * lock. Everything on disk stays with its driver.
 */

#define FAT_NAME_UNITS 255

/** UTF-8 to UTF-16, checked: 0 with `units` and `*count` filled, -EINVAL
 * for a name FAT cannot store (a forbidden character, a trailing dot or
 * space, `.` or `..`, or malformed UTF-8), -ENAMETOOLONG past 255 units
 * or 255 bytes. */
int fat_name_units(const char *name, uint16_t *units, uint32_t *count);

/** UTF-16 `units` (terminated by 0 or by `count`) as UTF-8 in `out`: 0,
 * -ENAMETOOLONG when it does not fit `cap`, -EINVAL for an unpaired
 * surrogate or a unit after the terminator that is not 0xFFFF padding. */
int fat_units_utf8(const uint16_t *units, uint32_t count, char *out, uint32_t cap);

/** The mount options both types take: `umask=`, `uid=`, `gid=` and `ro`;
 * 0 with the three filled (022, 0, 0 when absent), -EINVAL otherwise. */
int fat_parse_options(const char *options, uint32_t *uid, uint32_t *gid, uint32_t *umask);

/** The epoch second of a DOS date and time, read as UTC; a field out of
 * range reads as 1980-01-01. */
int64_t fat_epoch(uint16_t date, uint16_t time);

/** The DOS date and time of an epoch second, floored at 1980 and capped at
 * 2107; the time is rounded down to two seconds. */
void fat_stamp(int64_t sec, uint16_t *date, uint16_t *time);

/** The DOS date, time and hundredths of the present moment. */
void fat_now(uint16_t *date, uint16_t *time, uint8_t *tenth);

/** The inode number of an entry, the same policy for both types: a
 * directory is its first cluster; a file is its directory's cluster (0 for
 * the root) plus one in the high word and its entry index plus one in the
 * low, so no two entries share a number. */
uint64_t fat_inode(int is_dir, uint32_t first, uint32_t dir_cluster, uint32_t index);

/** Where a first-fit search starts: the hint when it names a cluster of
 * the volume, else the first cluster. */
uint32_t fat_hint_start(uint32_t hint, uint32_t cluster_count);

/** The cluster after `c`, wrapping to the first past the last. */
uint32_t fat_hint_next(uint32_t c, uint32_t cluster_count);

/** The one bitmap the dirty-volume walks share: 2^22 bits, one walk at a
 * time. `fat_walk_acquire` waits for the bitmap and clears the bits of
 * cluster numbers up to `cluster_count + 1`; `fat_walk_release` gives it
 * back. */
#define FAT_WALK_CLUSTERS (1u << 22)
void fat_walk_acquire(uint32_t cluster_count);
void fat_walk_release(void);

/** Mark cluster `c`: 1 if it was marked already, else 0. */
int fat_walk_test_set(uint32_t c);

/** Whether cluster `c` is marked. */
int fat_walk_marked(uint32_t c);

#endif /* _FATCOMMON_H_ */
