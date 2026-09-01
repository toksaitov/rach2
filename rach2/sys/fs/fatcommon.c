/*
 * fatcommon.c: what the FAT32 and exFAT drivers both need and neither owns,
 * as fatcommon.h says. The bodies came from fat32.c and fat32name.c
 * unchanged; the FAT32 tests are what say nothing moved wrongly.
 */

#include "fatcommon.h"

#include "../lib/kerrno.h"
#include "../lib/kstring.h"
#include "../proc/wait_queue.h"
#include "../rtc.h"

/* ---- names ---- */

static int forbidden(uint32_t cp)
{
    static const char set[] = "\"*/:<>?\\|";
    int i;

    if (cp < 0x20 || cp == 0x7F) {
        return 1;
    }
    for (i = 0; set[i]; i++) {
        if (cp == (uint32_t)(unsigned char)set[i]) {
            return 1;
        }
    }
    return 0;
}

int fat_name_units(const char *name, uint16_t *units, uint32_t *count)
{
    uint32_t n = 0;
    uint32_t i = 0;
    uint32_t last = 0;

    if (name[0] == '\0' ||
        (name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0')))) {
        return -EINVAL;
    }
    while (name[i]) {
        uint32_t c = (unsigned char)name[i];
        uint32_t cp, extra, k;

        if (c < 0x80) {
            cp = c;
            extra = 0;
        } else if ((c & 0xE0) == 0xC0) {
            cp = c & 0x1F;
            extra = 1;
        } else if ((c & 0xF0) == 0xE0) {
            cp = c & 0x0F;
            extra = 2;
        } else if ((c & 0xF8) == 0xF0) {
            cp = c & 0x07;
            extra = 3;
        } else {
            return -EINVAL;
        }
        for (k = 0; k < extra; k++) {
            uint32_t cc = (unsigned char)name[i + 1 + k];

            if ((cc & 0xC0) != 0x80) {
                return -EINVAL;
            }
            cp = (cp << 6) | (cc & 0x3F);
        }
        i += 1 + extra;
        if ((extra == 1 && cp < 0x80) || (extra == 2 && cp < 0x800) ||
            (extra == 3 && cp < 0x10000) || cp > 0x10FFFF || (cp >= 0xD800 && cp < 0xE000)) {
            return -EINVAL;
        }
        if (forbidden(cp)) {
            return -EINVAL;
        }
        if (cp >= 0x10000) {
            if (n + 2 > FAT_NAME_UNITS) {
                return -ENAMETOOLONG;
            }
            units[n++] = (uint16_t)(0xD800 + ((cp - 0x10000) >> 10));
            units[n++] = (uint16_t)(0xDC00 + ((cp - 0x10000) & 0x3FF));
        } else {
            if (n + 1 > FAT_NAME_UNITS) {
                return -ENAMETOOLONG;
            }
            units[n++] = (uint16_t)cp;
        }
        last = cp;
    }
    if (i > 255) {
        return -ENAMETOOLONG; /* the byte limit, beside the unit limit */
    }
    if (last == '.' || last == ' ') {
        return -EINVAL;
    }
    *count = n;
    return 0;
}

/* ---- timestamps ---- */

/* FAT keeps local time with no zone; it is read as UTC. Days count from
 * 1980-01-01 through the Gregorian rule, in 32-bit arithmetic. A field out
 * of its range reads as 1980-01-01, as an unset stamp does. */
int64_t fat_epoch(uint16_t date, uint16_t time)
{
    uint32_t year = 1980 + (date >> 9);
    uint32_t month = (date >> 5) & 0xF;
    uint32_t day = date & 0x1F;
    uint32_t days = 0;
    uint32_t yr;
    static const uint32_t before[12] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
    static const uint32_t length[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int leap;

    if (month < 1 || month > 12 || day < 1) {
        return 315532800; /* 1980-01-01: an unset or broken stamp */
    }
    leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
    if (day > length[month - 1] + (month == 2 && leap ? 1u : 0u) || (time >> 11) > 23 ||
        ((time >> 5) & 0x3F) > 59 || (time & 0x1F) > 29) {
        return 315532800;
    }
    for (yr = 1970; yr < year; yr++) {
        days += (yr % 4 == 0 && (yr % 100 != 0 || yr % 400 == 0)) ? 366 : 365;
    }
    days += before[month - 1] + day - 1;
    if (month > 2 && year % 4 == 0 && (year % 100 != 0 || year % 400 == 0)) {
        days++;
    }
    return (int64_t)days * 86400 + (int64_t)(time >> 11) * 3600 +
           (int64_t)((time >> 5) & 0x3F) * 60 + (int64_t)(time & 0x1F) * 2;
}

/* A FAT stamp from an epoch, the inverse of `fat_epoch`, floored at 1980
 * and capped at 2107. */
void fat_stamp(int64_t sec, uint16_t *date, uint16_t *time)
{
    uint32_t days, rem, year, month, day;
    static const uint32_t length[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

    if (sec < 315532800) {
        sec = 315532800;
    }
    if (sec > 4294967295LL) {
        sec = 4294967295LL;
    }
    days = (uint32_t)sec / 86400u;
    rem = (uint32_t)sec % 86400u;
    for (year = 1970;; year++) {
        uint32_t in_year = (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0)) ? 366 : 365;

        if (days < in_year) {
            break;
        }
        days -= in_year;
    }
    for (month = 1; month <= 12; month++) {
        uint32_t in_month = length[month - 1];

        if (month == 2 && year % 4 == 0 && (year % 100 != 0 || year % 400 == 0)) {
            in_month = 29;
        }
        if (days < in_month) {
            break;
        }
        days -= in_month;
    }
    day = days + 1;
    if (year > 2107) {
        year = 2107;
    }
    *date = (uint16_t)(((year - 1980) << 9) | (month << 5) | day);
    *time = (uint16_t)(((rem / 3600) << 11) | (((rem / 60) % 60) << 5) | ((rem % 60) / 2));
}

/* The clock as a FAT stamp: local time read as UTC, two-second resolution
 * in `time`, hundredths of the odd second in `tenth`. */
void fat_now(uint16_t *date, uint16_t *time, uint8_t *tenth)
{
    int64_t sec, nsec;
    uint32_t days, rem, year, month, day;
    static const uint32_t length[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

    rtc_realtime(&sec, &nsec);
    if (sec < 315532800) {
        sec = 315532800; /* before 1980: the stamp floors at the epoch FAT has */
    }
    if (sec > 4294967295LL) {
        sec = 4294967295LL; /* the year 2106, past FAT's own 2107 */
    }
    days = (uint32_t)sec / 86400u; /* 32-bit: the kernel has no 64-bit division */
    rem = (uint32_t)sec % 86400u;
    for (year = 1970;; year++) {
        uint32_t in_year = (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0)) ? 366 : 365;

        if (days < in_year) {
            break;
        }
        days -= in_year;
    }
    for (month = 1; month <= 12; month++) {
        uint32_t in_month = length[month - 1];

        if (month == 2 && year % 4 == 0 && (year % 100 != 0 || year % 400 == 0)) {
            in_month = 29;
        }
        if (days < in_month) {
            break;
        }
        days -= in_month;
    }
    day = days + 1;
    if (year > 2107) {
        year = 2107;
    }
    *date = (uint16_t)(((year - 1980) << 9) | (month << 5) | day);
    *time = (uint16_t)(((rem / 3600) << 11) | (((rem / 60) % 60) << 5) | ((rem % 60) / 2));
    if (nsec < 0 || nsec >= 1000000000) {
        nsec = 0;
    }
    *tenth = (uint8_t)(((rem % 60) % 2) * 100 + (uint32_t)nsec / 10000000u);
}

/* ---- the allocation hint ---- */

uint32_t fat_hint_start(uint32_t hint, uint32_t cluster_count)
{
    return hint >= 2 && hint < cluster_count + 2 ? hint : 2;
}

uint32_t fat_hint_next(uint32_t c, uint32_t cluster_count)
{
    return c + 1 < cluster_count + 2 ? c + 1 : 2;
}

/* ---- the dirty-volume walk's bitmap and its lock ---- */

static uint8_t walk_bits[FAT_WALK_CLUSTERS / 8];
static int walk_busy;
static wait_queue_t walk_wq;

static int walk_idle(void *arg)
{
    (void)arg;
    return !walk_busy;
}

int fat_walk_test_set(uint32_t c)
{
    uint8_t m = (uint8_t)(1u << (c & 7));

    if (walk_bits[c >> 3] & m) {
        return 1;
    }
    walk_bits[c >> 3] |= m;
    return 0;
}

void fat_walk_acquire(uint32_t cluster_count)
{
    while (walk_busy) {
        wq_wait_event_nointr(&walk_wq, walk_idle, 0);
    }
    walk_busy = 1;
    k_memset(walk_bits, 0, (cluster_count + 2 + 7) / 8);
}

void fat_walk_release(void)
{
    walk_busy = 0;
    wq_wake_all(&walk_wq);
}

int fat_walk_marked(uint32_t c)
{
    return (walk_bits[c >> 3] & (1u << (c & 7))) != 0;
}
