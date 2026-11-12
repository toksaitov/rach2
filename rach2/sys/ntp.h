/*
 * ntp.h: the clock's NTP state, Linux's, which adjtimex reads and changes,
 * the length of a tick it gives, which the clocks since boot and real time
 * run at, and the leap seconds it announces.
 */
#ifndef _NTP_H_
#define _NTP_H_

#include "lib/kint.h"

/* adjtimex's modes, Linux's (uapi/linux/timex.h and linux/timex.h). With
 * ADJ_ADJTIME, the call is adjtime(3)'s: ADJ_OFFSET_SINGLESHOT, which
 * ADJ_OFFSET_READONLY makes ADJ_OFFSET_SS_READ, and the bits are not the
 * others'. */
#define ADJ_OFFSET            0x0001u
#define ADJ_FREQUENCY         0x0002u
#define ADJ_MAXERROR          0x0004u
#define ADJ_ESTERROR          0x0008u
#define ADJ_STATUS            0x0010u
#define ADJ_TIMECONST         0x0020u
#define ADJ_TAI               0x0080u
#define ADJ_SETOFFSET         0x0100u
#define ADJ_MICRO             0x1000u
#define ADJ_NANO              0x2000u
#define ADJ_TICK              0x4000u
#define ADJ_ADJTIME           0x8000u
#define ADJ_OFFSET_READONLY   0x2000u
#define ADJ_OFFSET_SINGLESHOT 0x8001u
#define ADJ_OFFSET_SS_READ    0xa001u

/* The status bits and states, Linux's. */
#define STA_PLL      0x0001
#define STA_INS      0x0010
#define STA_DEL      0x0020
#define STA_FLL      0x0008
#define STA_UNSYNC   0x0040
#define STA_FREQHOLD 0x0080
#define STA_CLOCKERR 0x1000
#define STA_NANO     0x2000
#define STA_MODE     0x4000
#define STA_RONLY    0xff00 /* the PPS, error, nano, mode and clock bits */
#define TIME_OK      0
#define TIME_INS     1
#define TIME_DEL     2
#define TIME_OOP     3
#define TIME_WAIT    4
#define TIME_ERROR   5

/* The ticks adjtimex accepts, in microseconds of a tick of USER_HZ: Linux's
 * bounds, 10% either side of 10000. */
#define NTP_TICK_MIN 9000
#define NTP_TICK_MAX 11000

/* The largest frequency adjtimex takes, in its units of 2^-16 ppm, whose
 * product by Linux's PPM_SCALE, 1000 << 16, fits 64 bits. */
#define NTP_FREQ_LIMIT 140737488355LL

/* A timex in the kernel's own terms, which the three calls convert their
 * layouts to and from: the fields of Linux's struct __kernel_timex that
 * this kernel reads or sets. */
struct ktimex {
    uint32_t modes;
    int32_t status, shift, tai;
    int64_t offset, freq, maxerror, esterror, constant, precision, tolerance;
    int64_t sec, usec, tick;
};

/**
 * The NTP part of adjtimex, Linux's __do_adjtimex (kernel/time/ntp.c:
 * 769-853) without PPS: with ADJ_ADJTIME, adjtime(3)'s offset to slew, in
 * microseconds, and the one left read back; otherwise the modes of `tx`,
 * then its offset, frequency, errors, status, time constant, precision,
 * tolerance, tick and TAI offset filled in. `tx->sec` and `tx->usec` hold
 * the fine real time read before the call; `clock_sec` is real time's
 * seconds at the last timer pass, read under the clock's lock, in which
 * the PLL and FLL count their interval and reference time, as Linux's
 * __ktime_get_real_seconds (kernel/time/ntp.c:321-325, 708-709,
 * kernel/time/timekeeping.c:944-953). At or past a leap second the tick
 * has not yet made, the fine time and `tx->tai` read as the leap second's
 * own (ntp.c:835-850). The caller has checked the modes (syscall.c's
 * `ktimex_adjust`) and holds the
 * clock's lock; the next tick takes the tick length (`ntp_tick_length`).
 * Returns TIME_ERROR while STA_UNSYNC or STA_CLOCKERR is set, else the
 * leap second's state, TIME_OK while none is announced.
 */
int32_t ntp_adjtimex(struct ktimex *tx, int64_t clock_sec);

/**
 * The clock's second, Linux's second_overflow (kernel/time/ntp.c:400-495),
 * at the real time `secs`, the second that has just begun: a leap second
 * that STA_INS or STA_DEL announces is inserted or deleted at the end of
 * the day in UTC; the maximum error grows by the tolerance, and STA_UNSYNC
 * is set once it reaches its limit; and the next second's tick length is
 * the base, the frequency's, plus a share of the PLL's offset, which
 * shrinks by it, plus adjtime's slew of 500 ppm while one is left. Returns
 * the step real time takes, -1 for a second inserted, 1 for one deleted,
 * else 0; the TAI offset has moved the other way. The caller holds the
 * clock's lock.
 */
int ntp_second_overflow(int64_t secs);

/**
 * Forget an offset or adjtime's slew in progress, as a setting of the clock
 * does, Linux's ntp_clear (kernel/time/ntp.c:351-366): STA_UNSYNC set, the
 * errors at their limit, the tick length the frequency's alone, and no
 * leap second due. The caller holds the clock's lock.
 */
void ntp_clear(void);

/** The length of the next tick, in nanoseconds times 2^32, Linux's
 * tick_length: TICK_NS << 32 while nothing adjusts the clock. */
int64_t ntp_tick_length(void);

#endif /* _NTP_H_ */
