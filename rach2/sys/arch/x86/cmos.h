#ifndef _CMOS_H_
#define _CMOS_H_

#include "../../rtc.h"

#include "../../lib/kint.h"

/*
 * The PC's CMOS clock, an MC146818: a sample of its registers, and the
 * judgement and decoding of samples, which touch no hardware so that a host
 * test can run them. cmos.c also defines the reading, the setting and the
 * alarm of ../../rtc.h.
 */

/* One sample of the CMOS clock as it was read: the six date and time
 * registers, and status registers A, B and D, which say whether the clock
 * runs, how the six are encoded and whether the battery held. */
typedef struct {
    uint8_t second;
    uint8_t minute;
    uint8_t hour;
    uint8_t day;
    uint8_t month;
    uint8_t year;
    uint8_t status_a;
    uint8_t status_b;
    uint8_t status_d;
} rtc_snapshot_t;

/* Status register A: the divider field and its setting for a running clock,
 * and the flag that is set while the clock changes the registers. */
#define RTC_STATUS_A_DIVIDER        0x70
#define RTC_STATUS_A_DIVIDER_NORMAL 0x20
#define RTC_STATUS_A_UPDATING       0x80

/* Status register B: how the registers above are encoded, and whether they are
 * being maintained at all. */
#define RTC_STATUS_B_24_HOUR 0x02 /* set: hour is 0..23, clear: 1..12 with PM */
#define RTC_STATUS_B_BINARY  0x04 /* set: plain binary, clear: BCD */
#define RTC_STATUS_B_DSE     0x01 /* set: the clock adjusts itself for daylight */
#define RTC_STATUS_B_SET     0x80 /* set: updates are inhibited, time is frozen */

/* Status register D: the valid bit, clear once the battery has failed. */
#define RTC_STATUS_D_VALID 0x80

/* The PM flag: the hour register's high bit in 12-hour mode. */
#define RTC_HOUR_PM 0x80

/** Whether the sample `raw` says the clock lost its contents: status
 * register D's valid bit is clear. 1 when the contents are not to be
 * believed, a NULL sample included, 0 otherwise. */
int rtc_contents_lost(const rtc_snapshot_t *raw);

/** Whether two samples describe the same instant of the same clock: every
 * register equal, the three status registers included, but for status A's
 * update-in-progress bit. 0 when either is NULL. */
int rtc_snapshots_agree(const rtc_snapshot_t *a, const rtc_snapshot_t *b);

/* What a read does with a pair of samples: use it, take another pair, or
 * stop reading. */
#define RTC_PAIR_ACCEPT 0
#define RTC_PAIR_RETRY  1
#define RTC_PAIR_STOP   2

/** What the pair `a` and `b` is worth: RTC_PAIR_STOP when either says the
 * clock lost its contents, otherwise RTC_PAIR_RETRY when they disagree, and
 * RTC_PAIR_ACCEPT when they agree. */
int rtc_pair_disposition(const rtc_snapshot_t *a, const rtc_snapshot_t *b);

/**
 * Decode the sample `raw` into seconds since the Unix epoch, in UTC, at
 * `*seconds`, and into fields at `*out` when `out` is not NULL, which is
 * what the boot's log line prints. The year is 2000 plus the register's
 * two digits. 0, or -1 with nothing stored when `raw` or `seconds` is NULL,
 * the clock lost its contents, is not running (status A's divider, status
 * B's SET), keeps daylight time itself (status B's DSE, which the kernel
 * does not model), or has a field that is not a number in its encoding or
 * is out of range.
 */
int rtc_decode(const rtc_snapshot_t *raw, int64_t *seconds, rtc_time_t *out);

#endif /* _CMOS_H_ */
