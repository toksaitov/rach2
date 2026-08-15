/**
 * Dual-target seam test for the CMOS clock's arithmetic.
 *
 * host-deps: rach2/sys/rtc.c
 *
 * `rtc_decode`, `rtc_snapshots_agree` and `rtc_time_from_anchor` are the whole
 * of the clock's arithmetic and they touch no hardware, so they run natively
 * here over crafted samples: the two encodings, the 12-hour PM flag, a leap
 * year, the fields a dead battery reports, the two reads a boot compares before
 * it believes either, and the anchor a real time is measured from. The
 * expected seconds are literal, computed by hand rather than by the code under
 * test, because a case that derives its answer the way the implementation does
 * proves only that the implementation is self-consistent.
 *
 * Returns the failed-check count (0 == all passed), capped so it cannot wrap
 * to a false 0.
 */

#include "rtc.h"

static int fails;

#define CHECK(cond)    \
    do {               \
        if (!(cond)) { \
            fails++;   \
        }              \
    } while (0)

/** A BCD byte from a two-digit value, the way the CMOS holds one. */
static unsigned char bcd(int value)
{
    return (unsigned char)(((value / 10) << 4) | (value % 10));
}

/** Fill a BCD, 24-hour sample. */
static void bcd_sample(rtc_snapshot_t *raw, int year, int month, int day, int hour, int minute,
                       int second)
{
    raw->year = bcd(year);
    raw->month = bcd(month);
    raw->day = bcd(day);
    raw->hour = bcd(hour);
    raw->minute = bcd(minute);
    raw->second = bcd(second);
    raw->status_a = RTC_STATUS_A_DIVIDER_NORMAL;
    raw->status_b = RTC_STATUS_B_24_HOUR;
    raw->status_d = RTC_STATUS_D_VALID;
}

int main(void)
{
    rtc_snapshot_t raw;
    rtc_time_t when;
    long long seconds;

    /* The epoch itself, in the century this driver assumes: 2000-01-01 is
     * 10957 days after 1970-01-01, and 10957 * 86400 is 946684800. */
    bcd_sample(&raw, 0, 1, 1, 0, 0, 0);
    CHECK(rtc_decode(&raw, &seconds, &when) == 0);
    CHECK(seconds == 946684800LL);
    CHECK(when.year == 2000 && when.month == 1 && when.day == 1);

    /* Binary encoding of the same instant reads the same. */
    raw.year = 0;
    raw.month = 1;
    raw.day = 1;
    raw.hour = 0;
    raw.minute = 0;
    raw.second = 0;
    raw.status_a = RTC_STATUS_A_DIVIDER_NORMAL;
    raw.status_b = RTC_STATUS_B_24_HOUR | RTC_STATUS_B_BINARY;
    raw.status_d = RTC_STATUS_D_VALID;
    CHECK(rtc_decode(&raw, &seconds, &when) == 0);
    CHECK(seconds == 946684800LL);

    /* A leap day: 2024-02-29 12:00:00 UTC is 1709208000. A wrong leap rule
     * either rejects the date or lands a day out. */
    bcd_sample(&raw, 24, 2, 29, 12, 0, 0);
    CHECK(rtc_decode(&raw, &seconds, &when) == 0);
    CHECK(seconds == 1709208000LL);
    CHECK(when.day == 29 && when.hour == 12);

    /* 2100 is not a leap year, and 2000 was: the century rules differ. The
     * driver assumes the 2000s, so February 29 of a year ending in 00 is
     * reachable only as 2000, which must be accepted. */
    bcd_sample(&raw, 0, 2, 29, 0, 0, 0);
    CHECK(rtc_decode(&raw, &seconds, &when) == 0);

    /* Twelve-hour encoding. 1 PM is 13:00; the PM bit rides in the hour's high
     * bit and must come off before the BCD decode, or the hour reads as a
     * nonsense digit. 2026-08-14 13:05:07 UTC is 1786712707. */
    bcd_sample(&raw, 26, 8, 14, 1, 5, 7);
    raw.hour = (unsigned char)(bcd(1) | RTC_HOUR_PM);
    raw.status_b = 0; /* 12-hour, BCD */ /* 12-hour, BCD */
    CHECK(rtc_decode(&raw, &seconds, &when) == 0);
    CHECK(when.hour == 13);
    CHECK(seconds == 1786712707LL);

    /* 12 AM is hour 0 and 12 PM is hour 12, which is the case a plain
     * "add twelve for PM" gets wrong in both directions. */
    bcd_sample(&raw, 26, 8, 14, 12, 0, 0);
    raw.status_b = 0; /* 12-hour, BCD */
    CHECK(rtc_decode(&raw, &seconds, &when) == 0);
    CHECK(when.hour == 0);
    bcd_sample(&raw, 26, 8, 14, 12, 0, 0);
    raw.hour = (unsigned char)(bcd(12) | RTC_HOUR_PM);
    raw.status_b = 0; /* 12-hour, BCD */
    CHECK(rtc_decode(&raw, &seconds, &when) == 0);
    CHECK(when.hour == 12);

    /* Fields a clock with no battery reports. Each must be refused, not
     * corrected: a machine that believes it is month 0 is a machine that would
     * build everything or nothing. */
    bcd_sample(&raw, 26, 0, 14, 0, 0, 0);
    CHECK(rtc_decode(&raw, &seconds, &when) != 0);
    bcd_sample(&raw, 26, 13, 1, 0, 0, 0);
    CHECK(rtc_decode(&raw, &seconds, &when) != 0);
    bcd_sample(&raw, 26, 2, 30, 0, 0, 0);
    CHECK(rtc_decode(&raw, &seconds, &when) != 0);
    bcd_sample(&raw, 26, 8, 0, 0, 0, 0);
    CHECK(rtc_decode(&raw, &seconds, &when) != 0);
    bcd_sample(&raw, 26, 8, 14, 24, 0, 0);
    CHECK(rtc_decode(&raw, &seconds, &when) != 0);
    bcd_sample(&raw, 26, 8, 14, 0, 60, 0);
    CHECK(rtc_decode(&raw, &seconds, &when) != 0);

    /* A nibble that is not a digit is not a BCD number. 0x1A is what a byte
     * nobody maintains looks like, and it must fail rather than decode. */
    bcd_sample(&raw, 26, 8, 14, 0, 0, 0);
    raw.minute = 0x1A;
    CHECK(rtc_decode(&raw, &seconds, &when) != 0);

    /* A twelve-hour sample whose hour is 0 or 13 is not a twelve-hour time. */
    bcd_sample(&raw, 26, 8, 14, 0, 0, 0);
    raw.status_b = 0; /* 12-hour, BCD */
    CHECK(rtc_decode(&raw, &seconds, &when) != 0);
    bcd_sample(&raw, 26, 8, 14, 13, 0, 0);
    raw.status_b = 0; /* 12-hour, BCD */
    CHECK(rtc_decode(&raw, &seconds, &when) != 0);

    /* A NULL out-struct is allowed; the seconds still arrive. */
    bcd_sample(&raw, 0, 1, 1, 0, 0, 0);
    CHECK(rtc_decode(&raw, &seconds, (rtc_time_t *)0) == 0);
    CHECK(seconds == 946684800LL);

    /* What the clock says about ITSELF. Each of these is a sample whose date
     * fields are perfectly in range, which is exactly how a machine with a dead
     * battery reports: stable, plausible and wrong. */
    bcd_sample(&raw, 26, 8, 14, 12, 0, 0);
    raw.status_d = 0; /* VRT clear: the battery went */
    CHECK(rtc_decode(&raw, &seconds, &when) != 0);
    bcd_sample(&raw, 26, 8, 14, 12, 0, 0);
    raw.status_b |= RTC_STATUS_B_SET; /* updates inhibited: the clock is frozen */
    CHECK(rtc_decode(&raw, &seconds, &when) != 0);
    bcd_sample(&raw, 26, 8, 14, 12, 0, 0);
    raw.status_b |= RTC_STATUS_B_DSE; /* the clock shifts itself twice a year */
    CHECK(rtc_decode(&raw, &seconds, &when) != 0);
    bcd_sample(&raw, 26, 8, 14, 12, 0, 0);
    raw.status_a = 0; /* the divider is not the running one */
    CHECK(rtc_decode(&raw, &seconds, &when) != 0);

    /* The anchor. A clock read at tick 500 saying 1000, asked again at tick
     * 700, is 1002: two seconds later, not seven. Adding every tick since boot
     * would answer 1007 here, and on a real boot that error is under a second,
     * which is why it is stated as arithmetic rather than left to a tolerance. */
    {
        long long sec = 0;
        long long nsec = 0;

        rtc_time_from_anchor(1000LL, 5u, 7u, 0u, &sec, &nsec);
        CHECK(sec == 1002LL);
        CHECK(nsec == 0LL);

        /* Part of a second survives as nanoseconds: 55 ticks is 550 ms. */
        rtc_time_from_anchor(1000LL, 5u, 7u, 55u, &sec, &nsec);
        CHECK(sec == 1002LL);
        CHECK(nsec == 550000000LL);

        /* No anchor at all is time since boot, which is what a machine with no
         * readable clock reports and what this system reported before. */
        rtc_time_from_anchor(0LL, 0u, 2u, 50u, &sec, &nsec);
        CHECK(sec == 2LL);
        CHECK(nsec == 500000000LL);

        /* An uptime of a whole tick cycle. This is the case that made the
         * anchor count seconds rather than ticks: 2^32 ticks after the anchor,
         * a difference of TICKS is zero again and real time jumps back 497
         * days. Counted in seconds, 42949672 seconds later is 42949672 seconds
         * later. */
        rtc_time_from_anchor(1000LL, 0u, 42949672u, 0u, &sec, &nsec);
        CHECK(sec == 1000LL + 42949672LL);

        /* And the seconds counter's own wrap, 136 years out, still reads as
         * elapsed time rather than as a jump backwards. */
        rtc_time_from_anchor(1000LL, 0xFFFFFFF0u, 0x00000009u, 0u, &sec, &nsec);
        CHECK(sec == 1000LL + 25LL);
    }

    /* Two samples of one clock, which is what the boot read takes before it
     * decodes either. The registers advance while they are being read, so a
     * disagreement is a read to discard, and the status registers are part of
     * that: the decode judges the clock by what they say, so a divider that was
     * the running one in the first read and is not in the second must not be
     * accepted on the strength of the first. */
    {
        rtc_snapshot_t a;
        rtc_snapshot_t b;

        bcd_sample(&a, 26, 8, 14, 9, 31, 7);
        b = a;
        CHECK(rtc_snapshots_agree(&a, &b) == 1);

        /* The turn of a minute, which is the disagreement this is here for. */
        b.second = bcd(8);
        CHECK(rtc_snapshots_agree(&a, &b) == 0);

        /* Each status register in turn, since each is a reason to refuse. */
        b = a;
        b.status_a = 0;
        CHECK(rtc_snapshots_agree(&a, &b) == 0);
        b = a;
        b.status_b = (unsigned char)(a.status_b | RTC_STATUS_B_SET);
        CHECK(rtc_snapshots_agree(&a, &b) == 0);
        b = a;
        b.status_d = 0;
        CHECK(rtc_snapshots_agree(&a, &b) == 0);

        /* The update-in-progress flag is the exception, and has to be: it is
         * the one bit in those registers the hardware moves on its own, and it
         * moves once a second, so counting it would make every second read
         * disagree with itself for no reason. */
        b = a;
        b.status_a = (unsigned char)(a.status_a | RTC_STATUS_A_UPDATING);
        CHECK(rtc_snapshots_agree(&a, &b) == 1);

        CHECK(rtc_snapshots_agree(&a, 0) == 0);
        CHECK(rtc_snapshots_agree(0, &b) == 0);

        /* A battery that went ends the read rather than earning another try.
         * On some parts reading register D SETS the validity bit again, so the
         * pair a dead cell produces is a disagreement (clear, then set) and the
         * pair after it agrees, is in range, and is whatever the dead cell
         * left. The first sample of that sequence is the only usable one, and
         * this is what the driver stops on. */
        b = a;
        b.status_d = 0;
        CHECK(rtc_contents_lost(&b) == 1);
        CHECK(rtc_snapshots_agree(&b, &a) == 0);
        CHECK(rtc_contents_lost(&a) == 0);
        CHECK(rtc_contents_lost(0) == 1);
    }

    /* What the boot read does with a pair, which is the policy rather than the
     * comparison: a healthy pair is used, a straddled update is read again, and
     * a clock that says its battery went ends the read in either order. That
     * last one is why this is a function and not two lines in the loop: a pair
     * that both disagrees AND says the contents are lost must stop rather than
     * be read again, and nothing in the comparison alone says so. */
    {
        rtc_snapshot_t a;
        rtc_snapshot_t b;

        bcd_sample(&a, 26, 8, 14, 9, 31, 7);
        b = a;
        CHECK(rtc_pair_disposition(&a, &b) == RTC_PAIR_ACCEPT);

        b.status_a = (unsigned char)(a.status_a | RTC_STATUS_A_UPDATING);
        CHECK(rtc_pair_disposition(&a, &b) == RTC_PAIR_ACCEPT);

        b = a;
        b.second = bcd(8);
        CHECK(rtc_pair_disposition(&a, &b) == RTC_PAIR_RETRY);

        /* Clear then set, which is the pair a dead cell hands back when reading
         * register D sets the bit; and set then clear, the same fact arriving
         * in the other order. Both stop, and neither is retried. */
        b = a;
        b.status_d = 0;
        CHECK(rtc_pair_disposition(&b, &a) == RTC_PAIR_STOP);
        CHECK(rtc_pair_disposition(&a, &b) == RTC_PAIR_STOP);

        /* And the pair after it, which agrees and is in range and is whatever
         * the dead cell left: stopping on the valid pair is what keeps this
         * one from ever being taken. */
        b.status_d = 0;
        CHECK(rtc_pair_disposition(&b, &b) == RTC_PAIR_STOP);
    }

    return fails > 100 ? 100 : fails;
}
