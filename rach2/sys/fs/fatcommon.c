/*
 * fatcommon.c: what the FAT32 and exFAT drivers both need and neither owns,
 * as fatcommon.h says. The bodies came from fat32.c and fat32name.c
 * unchanged; the FAT32 tests are what say nothing moved wrongly.
 */

#include "fatcommon.h"

#include "../lib/kerrno.h"
#include "../lib/kstring.h"
#include "../mm/kheap.h"
#include "../mm/paging.h"
#include "../mm/pressure.h"
#include "../proc/wait_queue.h"
#include "../rtc.h"

/* ---- names ---- */

static int forbidden(uint32_t cp)
{
    static const char set[] = "\"*/:<>?\\|";
    int i;

    if (cp < 0x20) {
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

/* ---- names as UTF-8 ---- */

/* One UTF-16 code unit sequence into UTF-8; returns bytes written, 0 when
 * `cap` cannot hold it. Surrogate pairs arrive as two units. */
static int put_utf8(uint32_t cp, char *out, uint32_t cap)
{
    if (cp < 0x80) {
        if (cap < 1) {
            return 0;
        }
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        if (cap < 2) {
            return 0;
        }
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        if (cap < 3) {
            return 0;
        }
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    if (cap < 4) {
        return 0;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/**
 * Render UTF-16 `units` (terminated by 0 or by `count`) as UTF-8 into
 * `out`; 0 on success, -ENAMETOOLONG when it does not fit the component,
 * -EINVAL for a name no writer produces: an unpaired surrogate, or a unit
 * after the terminator that is not the 0xFFFF padding.
 */
int fat_units_utf8(const uint16_t *units, uint32_t count, char *out, uint32_t cap)
{
    uint32_t i = 0;
    uint32_t n = 0;

    while (i < count && units[i] != 0) {
        uint32_t cp = units[i++];
        int w;

        if (cp >= 0xD800 && cp < 0xDC00) {
            if (i >= count || units[i] < 0xDC00 || units[i] >= 0xE000) {
                return -EINVAL;
            }
            cp = 0x10000 + ((cp - 0xD800) << 10) + (units[i] - 0xDC00);
            i++;
        } else if (cp >= 0xDC00 && cp < 0xE000) {
            return -EINVAL;
        }
        w = put_utf8(cp, out + n, cap - 1 - n);
        if (w == 0) {
            return -ENAMETOOLONG;
        }
        n += (uint32_t)w;
    }
    for (i++; i < count; i++) {
        if (units[i] != 0xFFFF) {
            return -EINVAL;
        }
    }
    out[n] = '\0';
    return 0;
}

/* ---- the mount options ---- */

/* ---- the constructor ---- */

static int key_is(const char *key, const char *end, const char *word)
{
    while (key < end && *word && *key == *word) {
        key++;
        word++;
    }
    return key == end && *word == '\0';
}
/* `umask=`, `uid=` and `gid=` are the options Linux's vfat takes and
 * BusyBox passes through; `ro` is the flag given as data. Anything else
 * is refused. */
int fat_parse_options(const char *options, uint32_t *uid, uint32_t *gid, uint32_t *umask)
{
    const char *p = options ? options : "";

    *umask = 022;
    *uid = 0;
    *gid = 0;
    while (*p) {
        const char *key = p;
        const char *keyend;
        uint32_t value = 0;
        int base = 10;
        int digits = 0;

        int has_value = 0;

        while (*p && *p != ',' && *p != '=') {
            p++;
        }
        keyend = p;
        if (*p == '=') {
            p++;
            has_value = 1;
            if (key_is(key, keyend, "umask")) {
                base = 8;
            }
            while (*p >= '0' && *p < '0' + base) {
                uint32_t digit = (uint32_t)(*p - '0');

                if (value > (0xFFFFFFFFu - digit) / (uint32_t)base) {
                    return -EINVAL; /* past 32 bits */
                }
                value = value * (uint32_t)base + digit;
                p++;
                digits++;
            }
        }
        if (*p != '\0' && *p != ',') {
            return -EINVAL;
        }
        if (key_is(key, keyend, "ro") && !has_value) {
            /* the mount flag given as data */
        } else if (key_is(key, keyend, "umask") && digits) {
            *umask = value & 0777u;
        } else if (key_is(key, keyend, "uid") && digits) {
            *uid = value;
        } else if (key_is(key, keyend, "gid") && digits) {
            *gid = value;
        } else if (p == key) {
            /* an empty item, as in a trailing comma */
        } else {
            return -EINVAL;
        }
        if (*p == ',') {
            p++;
        }
    }
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

/* ---- identities ---- */

uint64_t fat_inode(int is_dir, uint32_t first, uint32_t dir_cluster, uint32_t index)
{
    if (is_dir) {
        return first;
    }
    return ((uint64_t)(dir_cluster + 1) << 32) | (uint64_t)(index + 1);
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

static uint8_t *walk_bits; /* held between acquire and release, else NULL */
static uint32_t walk_pages;
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

uint32_t fat_walk_bytes(uint32_t cluster_count)
{
    return (cluster_count + 2 + 7) / 8;
}

/**
 * The bitmap is taken in whole pages after the free frames are counted
 * against them and the reserve, since a walk that took the last frames
 * would starve the cache and the processes the check runs beside; the
 * caller mounts read-only and says so. The heap's own refusal is the
 * same answer.
 */
int fat_walk_acquire(uint32_t cluster_count)
{
    uint32_t pages = (fat_walk_bytes(cluster_count) + PAGE_SIZE - 1) / PAGE_SIZE;

    while (walk_busy) {
        wq_wait_event_nointr(&walk_wq, walk_idle, 0);
    }
    walk_busy = 1;
    if (frame_free_count() < pages + mem_reserve_frames()) {
        walk_bits = 0;
    } else {
        walk_bits = kalloc_pages(pages, 0);
    }
    if (!walk_bits) {
        walk_busy = 0;
        wq_wake_all(&walk_wq);
        return -ENOMEM;
    }
    walk_pages = pages;
    k_memset(walk_bits, 0, (uint32_t)pages * PAGE_SIZE);
    return 0;
}

void fat_walk_release(void)
{
    kfree_pages(walk_bits, walk_pages);
    walk_bits = 0;
    walk_pages = 0;
    walk_busy = 0;
    wq_wake_all(&walk_wq);
}

int fat_walk_marked(uint32_t c)
{
    return (walk_bits[c >> 3] & (1u << (c & 7))) != 0;
}
