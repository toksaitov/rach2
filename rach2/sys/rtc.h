#ifndef _RTC_H_
#define _RTC_H_

#include "lib/kint.h"

/*
 * One raw sample of the CMOS clock: the six date and time registers exactly as
 * they were read, plus status register B, which says how to read them. Kept as
 * a struct so the decoding below can be tested on a host that has no CMOS.
 */
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

/* Status register A: the divider that decides whether the clock runs at all,
 * and the flag the hardware raises while it is changing the registers. */
#define RTC_STATUS_A_DIVIDER        0x70
#define RTC_STATUS_A_DIVIDER_NORMAL 0x20
#define RTC_STATUS_A_UPDATING       0x80

/* Status register B: how the registers above are encoded, and whether they are
 * being maintained at all. */
#define RTC_STATUS_B_24_HOUR 0x02 /* set: hour is 0..23, clear: 1..12 with PM */
#define RTC_STATUS_B_BINARY  0x04 /* set: plain binary, clear: BCD */
#define RTC_STATUS_B_DSE     0x01 /* set: the clock adjusts itself for daylight */
#define RTC_STATUS_B_SET     0x80 /* set: updates are inhibited, time is frozen */

/* Status register D: whether the battery ever went. */
#define RTC_STATUS_D_VALID 0x80

/* The PM flag, which rides in the high bit of the hour in 12-hour mode. */
#define RTC_HOUR_PM 0x80

/* A sample decoded into fields, in UTC: what the clock meant, not what it
 * held. */
typedef struct {
    int32_t year;
    int32_t month;
    int32_t day;
    int32_t hour;
    int32_t minute;
    int32_t second;
} rtc_time_t;

int rtc_contents_lost(const rtc_snapshot_t *raw);

int rtc_snapshots_agree(const rtc_snapshot_t *a, const rtc_snapshot_t *b);

/* What the boot read does with a pair of samples. */
#define RTC_PAIR_ACCEPT 0
#define RTC_PAIR_RETRY  1
#define RTC_PAIR_STOP   2

int rtc_pair_disposition(const rtc_snapshot_t *a, const rtc_snapshot_t *b);

int rtc_decode(const rtc_snapshot_t *raw, int64_t *seconds, rtc_time_t *out);

void rtc_time_from_anchor(int64_t anchor_seconds, uint32_t anchor_uptime, uint32_t uptime,
                          uint32_t subsecond_ticks, int64_t *seconds, int64_t *nanoseconds);

void rtc_init(void);
int rtc_is_anchored(void);
int64_t rtc_boot_time(void);
int64_t rtc_boot_epoch(void);
void rtc_realtime(int64_t *seconds, int64_t *nanoseconds);
void rtc_coarse_realtime(int64_t *seconds, int64_t *nanoseconds);
void rtc_set_realtime(int64_t seconds, int32_t nanoseconds);

#endif
