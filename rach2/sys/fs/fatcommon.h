#ifndef _FATCOMMON_H_
#define _FATCOMMON_H_

#include "../lib/kint.h"

/*
 * fatcommon: what the FAT32 and exFAT drivers both need and neither owns:
 * the check and conversion of a name, the mount options, the DOS timestamp
 * bit fields, the inode numbers, the first-fit allocation hint, and the
 * dirty-volume walk's bitmap with its lock. Everything on disk stays with
 * its driver.
 */

/** The most UTF-16 units in a name. */
#define FAT_NAME_UNITS 255

/**
 * Convert the UTF-8 name `name` to UTF-16 in `units`, which has room for
 * `FAT_NAME_UNITS`, and check that FAT can store it.
 *
 * @return 0 with `units` and `*count` filled; -EINVAL for a name FAT cannot
 *         store (empty, `.` or `..`, a control or forbidden character, a
 *         trailing dot or space, or malformed or overlong UTF-8);
 *         -ENAMETOOLONG past 255 units or 255 bytes.
 */
int fat_name_units(const char *name, uint16_t *units, uint32_t *count);

/**
 * Convert the UTF-16 `units`, ended by a 0 or by `count`, to UTF-8 in `out`
 * of `cap` bytes, the terminating NUL included.
 *
 * @return 0; -ENAMETOOLONG when the name does not fit `cap`; -EINVAL for a
 *         name no writer produces: an unpaired surrogate, or a unit after
 *         the terminator that is not the 0xFFFF padding.
 */
int fat_units_utf8(const uint16_t *units, uint32_t count, char *out, uint32_t cap);

/**
 * Parse the mount options both types take, separated by commas: `umask=`
 * in octal, `uid=` and `gid=` in decimal, and `ro`. An empty item is
 * skipped and `options` may be NULL.
 *
 * @return 0 with the three filled, 022, 0 and 0 where absent, and the umask
 *         cut to 0777; -EINVAL for any other option, a value with no digits
 *         or past 32 bits, or `ro=`.
 */
int fat_parse_options(const char *options, uint32_t *uid, uint32_t *gid, uint32_t *umask);

/** The epoch second of a DOS date and time, read as UTC; a field out of
 * range reads as 1980-01-01. */
int64_t fat_epoch(uint16_t date, uint16_t time);

/** The DOS date and time of the epoch second `sec`, floored at 1980-01-01
 * and capped at 2106-02-07 06:28:15, the 32-bit limit; the time is rounded
 * down to two seconds. */
void fat_stamp(int64_t sec, uint16_t *date, uint16_t *time);

/** The DOS date and time of the present moment, as `fat_stamp` gives them,
 * and in `tenth` the hundredths of a second past the even second of
 * `time`, 0 to 199. */
void fat_now(uint16_t *date, uint16_t *time, uint8_t *tenth);

/** The inode number of an entry, the same policy for both types: a
 * directory is its first cluster; a file is its directory's cluster (0 for
 * the root) plus one in the high word and its entry index plus one in the
 * low, so no two entries share a number. */
uint64_t fat_inode(int is_dir, uint32_t first, uint32_t dir_cluster, uint32_t index);

/** Where a first-fit search starts: the hint when it names a cluster of
 * the volume, else the first cluster, 2. */
uint32_t fat_hint_start(uint32_t hint, uint32_t cluster_count);

/** The cluster after `c`, wrapping to the first past the last. */
uint32_t fat_hint_next(uint32_t c, uint32_t cluster_count);

/**
 * Take the bitmap the dirty-volume walks share, one bit for each cluster
 * number up to `cluster_count + 1`, all clear.
 *
 * One walk holds the bitmap at a time; a second caller waits, and a signal
 * does not end the wait. Process context.
 *
 * @return 0 with the bitmap held until `fat_walk_release`; -ENOMEM with
 *         nothing held when the pages it needs would leave the machine
 *         short.
 */
int fat_walk_acquire(uint32_t cluster_count);

/** The bytes of the bitmap for `cluster_count` clusters, for the caller
 * that reports a refusal of `fat_walk_acquire`. */
uint32_t fat_walk_bytes(uint32_t cluster_count);

/** Free the bitmap that `fat_walk_acquire` took and wake the next walk. */
void fat_walk_release(void);

/** Mark cluster `c` in the held bitmap: 1 if it was marked already, else
 * 0. */
int fat_walk_test_set(uint32_t c);

/** Whether cluster `c` is marked in the held bitmap. */
int fat_walk_marked(uint32_t c);

#endif /* _FATCOMMON_H_ */
