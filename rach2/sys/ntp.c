/*
 * ntp.c: the real-time clock's NTP state, as Linux 6.12.115's
 * kernel/time/ntp.c keeps it, without PPS and leap seconds: the phase-locked
 * and frequency-locked loops that adjtimex's offsets feed, the frequency,
 * adjtime's slew, and the length of a tick they give, which clock.c runs
 * real time at, so that an NTP daemon's corrections slew the clock and
 * never step it.
 */

#include "ntp.h"
#include "timer.h"

/* Linux's scales and limits (linux/timex.h:120-160). A tick length is in
 * nanoseconds times 2^NTP_SCALE_SHIFT, a tick being 1/NTP_INTERVAL_FREQ of a
 * second; a frequency in nanoseconds a second times 2^NTP_SCALE_SHIFT, and
 * in adjtimex's units of 2^-16 ppm times PPM_SCALE. */
#define NTP_SCALE_SHIFT     32
#define NTP_INTERVAL_FREQ   TICK_HZ
#define SHIFT_PLL           2
#define SHIFT_FLL           2
#define MAXTC               10
#define PPM_SCALE           ((int64_t)1000 << 16)
#define PPM_SCALE_INV_SHIFT 19
/* (1 << (PPM_SCALE_INV_SHIFT + NTP_SCALE_SHIFT)) / PPM_SCALE + 1, Linux's
 * PPM_SCALE_INV (linux/timex.h:131-133), which turns the frequency back
 * into adjtimex's units. */
#define PPM_SCALE_INV       34359739LL
#define MAXPHASE            500000000L /* nanoseconds */
#define MAXFREQ             500000L    /* nanoseconds a second */
#define MAXFREQ_SCALED      ((int64_t)MAXFREQ << NTP_SCALE_SHIFT)
#define MINSEC              256
#define MAXSEC              2048
#define NTP_PHASE_LIMIT     ((MAXPHASE / 1000) << 5) /* microseconds */
#define MAX_TAI_OFFSET      100000
/* adjtime's slew, 500 microseconds a second, and its share of a tick in
 * nanoseconds times 2^32, Linux's MAX_TICKADJ_SCALED; 500000 divides by
 * TICK_HZ, so the share is exact. */
#define MAX_TICKADJ         500L
#define MAX_TICKADJ_SCALED  ((int64_t)(MAX_TICKADJ * 1000L / NTP_INTERVAL_FREQ) << NTP_SCALE_SHIFT)

/* The state, Linux's of the same names (kernel/time/ntp.c:33-84). */
static uint32_t tick_usec = 10000; /* a tick of USER_HZ, microseconds */
static int64_t tick_length = (int64_t)TICK_NS << NTP_SCALE_SHIFT;
static int64_t tick_length_base = (int64_t)TICK_NS << NTP_SCALE_SHIFT;
static int32_t time_status = STA_UNSYNC;
static int64_t time_offset; /* the PLL's offset left, a tick's share */
static int32_t time_constant = 2;
static int64_t time_maxerror = NTP_PHASE_LIMIT; /* microseconds */
static int64_t time_esterror = NTP_PHASE_LIMIT; /* microseconds */
static int64_t time_freq;                       /* nanoseconds a second << 32 */
static int64_t time_reftime;                    /* real time at the last offset, seconds */
static int64_t time_adjust;                     /* adjtime's slew left, microseconds */
static int32_t time_tai;

/** `x` shifted right by `s`, toward zero, Linux's shift_right
 * (linux/timex.h:151-155). */
static int64_t shift_right(int64_t x, int s)
{
    return x < 0 ? -(int64_t)((uint64_t)-x >> s) : (int64_t)((uint64_t)x >> s);
}

/** `x` shifted right by `s`, toward minus infinity, as an arithmetic shift
 * of a signed value is on Linux's compilers. */
static int64_t shift_floor(int64_t x, int s)
{
    return x < 0 ? -(int64_t)(((uint64_t)-x - 1u) >> s) - 1 : (int64_t)((uint64_t)x >> s);
}

/** `n` divided by `d`, toward zero, Linux's div_s64, without libgcc
 * (`tick_div64`). */
static int64_t div_signed(int64_t n, uint32_t d)
{
    return n < 0 ? -(int64_t)tick_div64((uint64_t)-n, d, (uint64_t *)0)
                 : (int64_t)tick_div64((uint64_t)n, d, (uint64_t *)0);
}

/** `v` held to `lo` through `hi`. */
static int64_t clamp64(int64_t v, int64_t lo, int64_t hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

/**
 * The tick length's base from the tick and the frequency, and the tick
 * length moved by the change at once, Linux's ntp_update_frequency
 * (kernel/time/ntp.c:259-279).
 */
static void ntp_update_frequency(void)
{
    uint64_t second_length = ((uint64_t)tick_usec * 1000u * USER_HZ) << NTP_SCALE_SHIFT;
    int64_t new_base;

    second_length += (uint64_t)time_freq;
    new_base = (int64_t)tick_div(second_length, NTP_INTERVAL_FREQ, (uint32_t *)0);
    tick_length += new_base - tick_length_base;
    tick_length_base = new_base;
}

/**
 * The FLL's change of the frequency for an offset `offset64` of
 * nanoseconds measured `secs` seconds after the last, Linux's
 * ntp_update_offset_fll (kernel/time/ntp.c:281-294): none, in PLL mode,
 * unless the interval is long or STA_FLL asks for it.
 */
static int64_t ntp_update_offset_fll(int64_t offset64, int64_t secs)
{
    time_status &= ~STA_MODE;
    if (secs < MINSEC) {
        return 0;
    }
    if (!(time_status & STA_FLL) && secs <= MAXSEC) {
        return 0;
    }
    time_status |= STA_MODE;
    if (secs > 0xFFFFFFFFLL) {
        secs = 0xFFFFFFFFLL;
    }
    return div_signed(offset64 * ((int64_t)1 << (NTP_SCALE_SHIFT - SHIFT_FLL)), (uint32_t)secs);
}

/**
 * Take an offset of adjtimex's ADJ_OFFSET, in microseconds, or nanoseconds
 * with STA_NANO, measured at real time `now_sec`, Linux's ntp_update_offset
 * (kernel/time/ntp.c:296-346): with STA_PLL, the offset, at most half a
 * second, becomes the PLL's to slew, and the PLL and FLL move the
 * frequency by it.
 */
static void ntp_update_offset(int64_t offset, int64_t now_sec)
{
    int64_t freq_adj, secs;

    if (!(time_status & STA_PLL)) {
        return;
    }
    if (!(time_status & STA_NANO)) {
        offset = clamp64(offset, -1000000, 1000000) * 1000;
    }
    offset = clamp64(offset, -MAXPHASE, MAXPHASE);
    secs = now_sec - time_reftime;
    if (time_status & STA_FREQHOLD) {
        secs = 0;
    }
    time_reftime = now_sec;
    freq_adj = ntp_update_offset_fll(offset, secs);
    if (secs > ((int64_t)1 << (SHIFT_PLL + 1 + time_constant))) {
        secs = (int64_t)1 << (SHIFT_PLL + 1 + time_constant);
    }
    freq_adj +=
        offset * secs * ((int64_t)1 << (NTP_SCALE_SHIFT - 2 * (SHIFT_PLL + 2 + time_constant)));
    freq_adj += time_freq;
    if (freq_adj > MAXFREQ_SCALED) {
        freq_adj = MAXFREQ_SCALED;
    }
    time_freq = freq_adj < -MAXFREQ_SCALED ? -MAXFREQ_SCALED : freq_adj;
    time_offset = div_signed(offset * ((int64_t)1 << NTP_SCALE_SHIFT), NTP_INTERVAL_FREQ);
}

void ntp_clear(void)
{
    time_adjust = 0;
    time_status |= STA_UNSYNC;
    time_maxerror = NTP_PHASE_LIMIT;
    time_esterror = NTP_PHASE_LIMIT;
    ntp_update_frequency();
    tick_length = tick_length_base;
    time_offset = 0;
}

int64_t ntp_tick_length(void)
{
    return tick_length;
}

/**
 * Leap seconds are not kept, so the state stays TIME_OK and STA_INS and
 * STA_DEL change nothing. The PLL's share is its offset shifted by
 * SHIFT_PLL and the time constant, a quarter of it at the least; adjtime's
 * slew takes 500 microseconds a second until less is left, and then the
 * rest (kernel/time/ntp.c:457-491).
 */
void ntp_second_overflow(void)
{
    int64_t delta;

    time_maxerror += MAXFREQ / 1000;
    if (time_maxerror > NTP_PHASE_LIMIT) {
        time_maxerror = NTP_PHASE_LIMIT;
        time_status |= STA_UNSYNC;
    }
    tick_length = tick_length_base;
    delta = shift_right(time_offset, SHIFT_PLL + time_constant);
    time_offset -= delta;
    tick_length += delta;
    if (time_adjust == 0) {
        return;
    }
    if (time_adjust > MAX_TICKADJ) {
        time_adjust -= MAX_TICKADJ;
        tick_length += MAX_TICKADJ_SCALED;
        return;
    }
    if (time_adjust < -MAX_TICKADJ) {
        time_adjust += MAX_TICKADJ;
        tick_length -= MAX_TICKADJ_SCALED;
        return;
    }
    tick_length +=
        div_signed(time_adjust * 1000, NTP_INTERVAL_FREQ) * ((int64_t)1 << NTP_SCALE_SHIFT);
    time_adjust = 0;
}

/**
 * Take ADJ_STATUS's bits but the read-only ones, Linux's process_adj_status
 * (kernel/time/ntp.c:694-714): turning STA_PLL off leaves the status
 * STA_UNSYNC alone, and turning it on starts the PLL's interval at
 * `now_sec`.
 */
static void ntp_take_status(const struct ktimex *tx, int64_t now_sec)
{
    if ((time_status & STA_PLL) && !(tx->status & STA_PLL)) {
        time_status = STA_UNSYNC;
    }
    if (!(time_status & STA_PLL) && (tx->status & STA_PLL)) {
        time_reftime = now_sec;
    }
    time_status &= STA_RONLY;
    time_status |= tx->status & ~STA_RONLY;
}

/** Take the modes of `tx`, Linux's process_adjtimex_modes
 * (kernel/time/ntp.c:717-762). */
static void ntp_take_modes(const struct ktimex *tx, int64_t now_sec)
{
    uint32_t m = tx->modes;

    if (m & ADJ_STATUS) {
        ntp_take_status(tx, now_sec);
    }
    if (m & ADJ_NANO) {
        time_status |= STA_NANO;
    }
    if (m & ADJ_MICRO) {
        time_status &= ~STA_NANO;
    }
    if (m & ADJ_FREQUENCY) {
        time_freq = clamp64(tx->freq * PPM_SCALE, -MAXFREQ_SCALED, MAXFREQ_SCALED);
    }
    if (m & ADJ_MAXERROR) {
        time_maxerror = clamp64(tx->maxerror, 0, NTP_PHASE_LIMIT);
    }
    if (m & ADJ_ESTERROR) {
        time_esterror = clamp64(tx->esterror, 0, NTP_PHASE_LIMIT);
    }
    if (m & ADJ_TIMECONST) {
        int64_t tc = clamp64(tx->constant, 0, MAXTC);

        if (!(time_status & STA_NANO)) {
            tc += 4;
        }
        time_constant = (int32_t)clamp64(tc, 0, MAXTC);
    }
    if ((m & ADJ_TAI) && tx->constant >= 0 && tx->constant <= MAX_TAI_OFFSET) {
        time_tai = (int32_t)tx->constant;
    }
    if (m & ADJ_OFFSET) {
        ntp_update_offset(tx->offset, now_sec);
    }
    if (m & ADJ_TICK) {
        tick_usec = (uint32_t)tx->tick;
    }
    if (m & (ADJ_TICK | ADJ_FREQUENCY | ADJ_OFFSET)) {
        ntp_update_frequency();
    }
}

int32_t ntp_adjtimex(struct ktimex *tx, int64_t now_sec)
{
    if (tx->modes & ADJ_ADJTIME) {
        int64_t save_adjust = time_adjust;

        if (!(tx->modes & ADJ_OFFSET_READONLY)) {
            time_adjust = tx->offset;
            ntp_update_frequency();
        }
        tx->offset = save_adjust;
    } else {
        if (tx->modes != 0) {
            ntp_take_modes(tx, now_sec);
        }
        tx->offset = shift_right(time_offset * NTP_INTERVAL_FREQ, NTP_SCALE_SHIFT);
        if (!(time_status & STA_NANO)) {
            tx->offset = div_signed(tx->offset, 1000);
        }
    }
    tx->freq =
        shift_right(shift_floor(time_freq, PPM_SCALE_INV_SHIFT) * PPM_SCALE_INV, NTP_SCALE_SHIFT);
    tx->maxerror = time_maxerror;
    tx->esterror = time_esterror;
    tx->status = time_status;
    tx->constant = time_constant;
    tx->precision = 1;
    tx->tolerance = (int64_t)MAXFREQ / 1000 << 16; /* MAXFREQ_SCALED / PPM_SCALE */
    tx->tick = tick_usec;
    tx->shift = 0;
    tx->tai = time_tai;
    return (time_status & (STA_UNSYNC | STA_CLOCKERR)) ? TIME_ERROR : TIME_OK;
}
