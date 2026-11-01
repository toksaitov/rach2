/*
 * ani.c: the ath9k driver's adaptive noise immunity.
 *
 * The baseband counts the OFDM and CCK frames it began to receive and
 * then lost, the phy errors. Many errors a second mean the receiver
 * listens to noise, so ANI raises an immunity level: the spur immunity
 * and the FIR step move up together, and the receiver hears less of the
 * weakest signals but stops being deafened by the noise. Few errors lower
 * the levels again, one kind a turn. Each level is a row of ath9k's
 * tables, written relative to the initial-value tables' own settings,
 * which the driver reads after each reset. rach2.ath9k_noani on the boot
 * line leaves the levels at the tables' defaults, since a rate stuck at 1
 * Mbit/s has been reported with ANI on.
 *
 * The functions follow ath9k's ani.c, ar9003_phy.c (ar9003_hw_ani_control
 * and ar9003_hw_ani_cache_ini_regs) and ath/hw.c (the cycle counters),
 * each named at its function, under ath9k's notice:
 *
 * Copyright (c) 2008-2011 Atheros Communications Inc.
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "ath9k.h"
#include "../../../cmdline.h"

/* The trigger levels in phy errors per second (ani.h): OFDM's when its
 * level is at the default or above, and below it. */
#define OFDM_TRIG_HIGH           3500
#define OFDM_TRIG_HIGH_BELOW_INI 1000
#define OFDM_TRIG_LOW            400
#define OFDM_TRIG_LOW_ABOVE_INI  900
#define CCK_TRIG_HIGH            600
#define CCK_TRIG_LOW             300

/* The default levels, which the tables' settings are (ani.h, ani.c). */
#define SPUR_IMMUNE_DEFAULT 3
#define FIRSTEP_DEFAULT     2
#define OFDM_DEFAULT        3
#define CCK_DEFAULT         2

/* The beacons' RSSI below which weak OFDM signals stay detected, and
 * below which CCK immunity stops short of its two highest levels. */
#define RSSI_THR_HIGH 40
#define RSSI_THR_LOW  7

/* The listening time, in milliseconds, before the levels move. */
#define ANI_PERIOD 300

/* The bounds of the FIR step and spur immunity fields (ani.h). */
#define FIRSTEP_SETTING_MAX  20
#define SPUR_IMM_SETTING_MAX 22

/* The MAC's clock on a 2.4 GHz OFDM channel in MHz, which the cycle
 * counters count (hw.h's ATH9K_CLOCK_RATE_2GHZ_OFDM). */
#define CLOCK_RATE_2G 44

/* The FIR step and the spur immunity's cycle power threshold of each
 * level, relative to the default level (ar9003_phy.c). */
static const int firstep_table[] = {-4, -2, 0, 2, 4, 6, 8, 10, 12};
static const int cycpwr_thr1_table[] = {-6, -4, -2, 0, 2, 4, 6, 8};

/* The OFDM weak signal fields' values that turn the detection off. */
#define M1_THRESH_LOW_OFF    127
#define M2_THRESH_LOW_OFF    127
#define M1_THRESH_OFF        127
#define M2_THRESH_OFF        127
#define M2_COUNT_THR_OFF     31
#define M2_COUNT_THR_LOW_OFF 63

/* An OFDM immunity level: its spur immunity and FIR step levels, and
 * whether weak signals are detected (ani.c's ofdm_level_table). */
struct ofdm_level {
    uint8_t spur;
    uint8_t firstep;
    uint8_t weak_signal;
};

static const struct ofdm_level ofdm_levels[] = {{0, 0, 1}, {1, 1, 1}, {2, 2, 1}, {3, 2, 1},
                                                {4, 3, 1}, {5, 4, 1}, {6, 5, 1}, {7, 6, 1},
                                                {7, 7, 1}, {7, 8, 0}};

/* A CCK immunity level: its FIR step level and whether CCK frames combine
 * the chains (ani.c's cck_level_table). */
struct cck_level {
    uint8_t firstep;
    uint8_t mrc_cck;
};

static const struct cck_level cck_levels[] = {{0, 1}, {1, 1}, {2, 1}, {3, 1}, {4, 0},
                                              {5, 0}, {6, 0}, {7, 0}, {8, 0}};

#define OFDM_MAX_LEVEL         (sizeof(ofdm_levels) / sizeof(ofdm_levels[0]) - 1)
#define CCK_MAX_LEVEL          (sizeof(cck_levels) / sizeof(cck_levels[0]) - 1)
#define CCK_MAX_LEVEL_LOW_RSSI (CCK_MAX_LEVEL - 2)

/** `v` limited to 0 to `max`. */
static uint32_t clamp(int v, int max)
{
    return (uint32_t)(v < 0 ? 0 : v > max ? max : v);
}

/* ------------------------------------------------------------------ */
/* The baseband's fields (ar9003_phy.c).                               */
/* ------------------------------------------------------------------ */

/**
 * Turn the detection of weak OFDM signals on or off, as
 * ar9003_hw_ani_control's ATH9K_ANI_OFDM_WEAK_SIGNAL_DETECTION does: the
 * thresholds of the tables or their "off" values, which a chip of the
 * AR9462's design leaves alone, then the self-correlation.
 */
static void set_weak_signal(struct ath9k *ah, int on)
{
    struct ath9k_ani *a = &ah->ani;

    if (!(ah->hw.chip->caps & ATH9K_CHIP_JUPITER)) {
        REG_RMW_FIELD(ah, AR_PHY_SFCORR_LOW, AR_PHY_SFCORR_LOW_M1_THRESH_LOW,
                      on ? a->m1_thresh_low : M1_THRESH_LOW_OFF);
        REG_RMW_FIELD(ah, AR_PHY_SFCORR_LOW, AR_PHY_SFCORR_LOW_M2_THRESH_LOW,
                      on ? a->m2_thresh_low : M2_THRESH_LOW_OFF);
        REG_RMW_FIELD(ah, AR_PHY_SFCORR, AR_PHY_SFCORR_M1_THRESH,
                      on ? a->m1_thresh : M1_THRESH_OFF);
        REG_RMW_FIELD(ah, AR_PHY_SFCORR, AR_PHY_SFCORR_M2_THRESH,
                      on ? a->m2_thresh : M2_THRESH_OFF);
        REG_RMW_FIELD(ah, AR_PHY_SFCORR, AR_PHY_SFCORR_M2COUNT_THR,
                      on ? a->m2_count_thr : M2_COUNT_THR_OFF);
        REG_RMW_FIELD(ah, AR_PHY_SFCORR_LOW, AR_PHY_SFCORR_LOW_M2COUNT_THR_LOW,
                      on ? a->m2_count_thr_low : M2_COUNT_THR_LOW_OFF);
        REG_RMW_FIELD(ah, AR_PHY_SFCORR_EXT, AR_PHY_SFCORR_EXT_M1_THRESH_LOW,
                      on ? a->m1_thresh_low_ext : M1_THRESH_LOW_OFF);
        REG_RMW_FIELD(ah, AR_PHY_SFCORR_EXT, AR_PHY_SFCORR_EXT_M2_THRESH_LOW,
                      on ? a->m2_thresh_low_ext : M2_THRESH_LOW_OFF);
        REG_RMW_FIELD(ah, AR_PHY_SFCORR_EXT, AR_PHY_SFCORR_EXT_M1_THRESH,
                      on ? a->m1_thresh_ext : M1_THRESH_OFF);
        REG_RMW_FIELD(ah, AR_PHY_SFCORR_EXT, AR_PHY_SFCORR_EXT_M2_THRESH,
                      on ? a->m2_thresh_ext : M2_THRESH_OFF);
    }
    if (on)
        REG_SET_BIT(ah, AR_PHY_SFCORR_LOW, AR_PHY_SFCORR_LOW_USE_SELF_CORR_LOW);
    else
        REG_CLR_BIT(ah, AR_PHY_SFCORR_LOW, AR_PHY_SFCORR_LOW_USE_SELF_CORR_LOW);
    a->ofdm_weak_signal = (uint8_t)(on != 0);
}

/** Set the FIR step of `level`, relative to the tables' settings, as
 * ar9003_hw_ani_control's ATH9K_ANI_FIRSTEP_LEVEL does. */
static void set_firstep(struct ath9k *ah, unsigned level)
{
    struct ath9k_ani *a = &ah->ani;
    int delta = firstep_table[level] - firstep_table[FIRSTEP_DEFAULT];

    REG_RMW_FIELD(ah, AR_PHY_FIND_SIG, AR_PHY_FIND_SIG_FIRSTEP,
                  clamp(delta + a->firstep, FIRSTEP_SETTING_MAX));
    REG_RMW_FIELD(ah, AR_PHY_FIND_SIG_LOW, AR_PHY_FIND_SIG_LOW_FIRSTEP_LOW,
                  clamp(delta + a->firstep_low, FIRSTEP_SETTING_MAX));
    a->firstep_level = (uint8_t)level;
}

/** Set the spur immunity of `level`, relative to the tables' settings, as
 * ar9003_hw_ani_control's ATH9K_ANI_SPUR_IMMUNITY_LEVEL does. */
static void set_spur_immunity(struct ath9k *ah, unsigned level)
{
    struct ath9k_ani *a = &ah->ani;
    int delta = cycpwr_thr1_table[level] - cycpwr_thr1_table[SPUR_IMMUNE_DEFAULT];

    REG_RMW_FIELD(ah, AR_PHY_TIMING5, AR_PHY_TIMING5_CYCPWR_THR1,
                  clamp(delta + a->cycpwr_thr1, SPUR_IMM_SETTING_MAX));
    REG_RMW_FIELD(ah, AR_PHY_EXT_CCA, AR_PHY_EXT_CYCPWR_THR1,
                  clamp(delta + a->cycpwr_thr1_ext, SPUR_IMM_SETTING_MAX));
    a->spur_level = (uint8_t)level;
}

/** Turn the combining of the chains for CCK on or off, as
 * ar9003_hw_ani_control's ATH9K_ANI_MRC_CCK does; one chain has nothing
 * to combine. */
static void set_mrc_cck(struct ath9k *ah, int on)
{
    if (ah->hw.chip->chainmask == 1)
        return;
    REG_RMW_FIELD(ah, AR_PHY_MRC_CCK_CTRL, AR_PHY_MRC_CCK_ENABLE, on != 0);
    REG_RMW_FIELD(ah, AR_PHY_MRC_CCK_CTRL, AR_PHY_MRC_CCK_MUX_REG, on != 0);
    ah->ani.mrc_cck = (uint8_t)(on != 0);
}

/** Read the tables' settings of the fields ANI moves, which the levels
 * are relative to, and take the levels as the defaults the tables set, as
 * ar9003_hw_ani_cache_ini_regs does. */
static void cache_ini_regs(struct ath9k *ah)
{
    struct ath9k_ani *a = &ah->ani;
    uint32_t v;

    v = REG_READ(ah, AR_PHY_SFCORR);
    a->m1_thresh = (uint8_t)MS(v, AR_PHY_SFCORR_M1_THRESH);
    a->m2_thresh = (uint8_t)MS(v, AR_PHY_SFCORR_M2_THRESH);
    a->m2_count_thr = (uint8_t)MS(v, AR_PHY_SFCORR_M2COUNT_THR);
    v = REG_READ(ah, AR_PHY_SFCORR_LOW);
    a->m1_thresh_low = (uint8_t)MS(v, AR_PHY_SFCORR_LOW_M1_THRESH_LOW);
    a->m2_thresh_low = (uint8_t)MS(v, AR_PHY_SFCORR_LOW_M2_THRESH_LOW);
    a->m2_count_thr_low = (uint8_t)MS(v, AR_PHY_SFCORR_LOW_M2COUNT_THR_LOW);
    v = REG_READ(ah, AR_PHY_SFCORR_EXT);
    a->m1_thresh_ext = (uint8_t)MS(v, AR_PHY_SFCORR_EXT_M1_THRESH);
    a->m2_thresh_ext = (uint8_t)MS(v, AR_PHY_SFCORR_EXT_M2_THRESH);
    a->m1_thresh_low_ext = (uint8_t)MS(v, AR_PHY_SFCORR_EXT_M1_THRESH_LOW);
    a->m2_thresh_low_ext = (uint8_t)MS(v, AR_PHY_SFCORR_EXT_M2_THRESH_LOW);
    a->firstep = (uint8_t)REG_READ_FIELD(ah, AR_PHY_FIND_SIG, AR_PHY_FIND_SIG_FIRSTEP);
    a->firstep_low =
        (uint8_t)REG_READ_FIELD(ah, AR_PHY_FIND_SIG_LOW, AR_PHY_FIND_SIG_LOW_FIRSTEP_LOW);
    a->cycpwr_thr1 = (uint8_t)REG_READ_FIELD(ah, AR_PHY_TIMING5, AR_PHY_TIMING5_CYCPWR_THR1);
    a->cycpwr_thr1_ext = (uint8_t)REG_READ_FIELD(ah, AR_PHY_EXT_CCA, AR_PHY_EXT_CYCPWR_THR1);

    a->spur_level = SPUR_IMMUNE_DEFAULT;
    a->firstep_level = FIRSTEP_DEFAULT;
    a->ofdm_weak_signal = 1;
    a->mrc_cck = 1;
}

/* ------------------------------------------------------------------ */
/* The levels (ani.c).                                                 */
/* ------------------------------------------------------------------ */

/**
 * Set OFDM immunity to `level`, as ath9k_hw_set_ofdm_nil does for a
 * station: its spur immunity; its FIR step, unless CCK's level asks for
 * more; weak signals detected while the beacons are weak; and the trigger
 * levels for the errors, which are lower below the default level.
 */
static void set_ofdm_level(struct ath9k *ah, unsigned level)
{
    struct ath9k_ani *a = &ah->ani;
    const struct ofdm_level *o;
    const struct cck_level *c;
    int weak;

    a->ofdm_level = (uint8_t)level;
    o = &ofdm_levels[a->ofdm_level];
    c = &cck_levels[a->cck_level];
    if (a->spur_level != o->spur)
        set_spur_immunity(ah, o->spur);
    if (a->firstep_level != o->firstep && o->firstep >= c->firstep)
        set_firstep(ah, o->firstep);
    weak = o->weak_signal || a->beacon_rssi <= RSSI_THR_HIGH;
    if (a->ofdm_weak_signal != weak)
        set_weak_signal(ah, weak);
    if (a->ofdm_level >= OFDM_DEFAULT) {
        a->ofdm_trig_high = OFDM_TRIG_HIGH;
        a->ofdm_trig_low = OFDM_TRIG_LOW_ABOVE_INI;
    } else {
        a->ofdm_trig_high = OFDM_TRIG_HIGH_BELOW_INI;
        a->ofdm_trig_low = OFDM_TRIG_LOW;
    }
}

/**
 * Set CCK immunity to `level`, as ath9k_hw_set_cck_nil does for a
 * station: no higher than two below the top while the beacons are weak;
 * its FIR step, unless OFDM's level asks for more; and the combining of
 * the chains, which ath9k leaves alone on one-chain chips.
 */
static void set_cck_level(struct ath9k *ah, unsigned level)
{
    struct ath9k_ani *a = &ah->ani;
    const struct ofdm_level *o;
    const struct cck_level *c;

    if (a->beacon_rssi <= RSSI_THR_LOW && level > CCK_MAX_LEVEL_LOW_RSSI)
        level = CCK_MAX_LEVEL_LOW_RSSI;
    a->cck_level = (uint8_t)level;
    o = &ofdm_levels[a->ofdm_level];
    c = &cck_levels[a->cck_level];
    if (a->firstep_level != c->firstep && c->firstep >= o->firstep)
        set_firstep(ah, c->firstep);
    if (ah->hw.chip->chainmask != 1 && a->mrc_cck != c->mrc_cck)
        set_mrc_cck(ah, c->mrc_cck);
}

/** Start counting the errors over, as ath9k_ani_restart does. */
static void restart(struct ath9k *ah)
{
    ah->ani.listen_time = 0;
    REG_WRITE(ah, AR_PHY_ERR_1, 0);
    REG_WRITE(ah, AR_PHY_ERR_2, 0);
    REG_WRITE(ah, AR_PHY_ERR_MASK_1, AR_PHY_ERR_OFDM_TIMING);
    REG_WRITE(ah, AR_PHY_ERR_MASK_2, AR_PHY_ERR_CCK_TIMING);
    ah->ani.ofdm_errors = 0;
    ah->ani.cck_errors = 0;
}

/** Turn the MIB counters on, counting OFDM and CCK timing errors, as
 * ath9k_enable_mib_counters does. */
static void enable_mib_counters(struct ath9k *ah)
{
    REG_WRITE(ah, AR_FILT_OFDM, 0);
    REG_WRITE(ah, AR_FILT_CCK, 0);
    REG_WRITE(ah, AR_MIBC, ~(AR_MIBC_COW | AR_MIBC_FMC | AR_MIBC_CMC | AR_MIBC_MCS) & 0x0f);
    REG_WRITE(ah, AR_PHY_ERR_MASK_1, AR_PHY_ERR_OFDM_TIMING);
    REG_WRITE(ah, AR_PHY_ERR_MASK_2, AR_PHY_ERR_CCK_TIMING);
}

/**
 * Set the levels again after a reset, as ath9k_ani_reset does for a
 * station: the default levels the first time, as ath9k_hw_ani_init sets
 * them, and the higher of the default and the level held after it; the
 * tables' settings read first, and the counts restarted. The first call
 * sets ATH9K_OPT_NO_ANI when rach2.ath9k_noani is on the boot line; under
 * it no level ever leaves its default.
 */
void ath9k_ani_reset(struct ath9k *ah)
{
    struct ath9k_ani *a = &ah->ani;
    unsigned ofdm, cck;

    if (!a->ready) {
        if (kcmdline_has("rach2.ath9k_noani"))
            ah->options |= ATH9K_OPT_NO_ANI;
        a->ofdm_trig_high = OFDM_TRIG_HIGH;
        a->ofdm_trig_low = OFDM_TRIG_LOW;
        a->cck_trig_high = CCK_TRIG_HIGH;
        a->cck_trig_low = CCK_TRIG_LOW;
        a->ofdms_turn = 1;
        a->ofdm_level = OFDM_DEFAULT;
        a->cck_level = CCK_DEFAULT;
        a->ready = 1;
    }
    cache_ini_regs(ah);
    ofdm = a->ofdm_level > OFDM_DEFAULT ? a->ofdm_level : OFDM_DEFAULT;
    cck = a->cck_level > CCK_DEFAULT ? a->cck_level : CCK_DEFAULT;
    set_ofdm_level(ah, ofdm);
    set_cck_level(ah, cck);
    restart(ah);
    enable_mib_counters(ah);
}

/**
 * Add the time since the last read and the error counts, as
 * ath9k_hw_ani_read_counters does: the cycle counters frozen, read and
 * cleared, the listening time the cycles neither receiving nor
 * transmitting, at 44 MHz. Returns 0, the counts restarted, when no time
 * was heard.
 */
static int read_counters(struct ath9k *ah)
{
    struct ath9k_ani *a = &ah->ani;
    uint32_t cycles, rx, tx;
    int32_t listen;

    REG_WRITE(ah, AR_MIBC, AR_MIBC_FMC);
    cycles = REG_READ(ah, AR_CCCNT);
    (void)REG_READ(ah, AR_RCCNT);
    rx = REG_READ(ah, AR_RFCNT);
    tx = REG_READ(ah, AR_TFCNT);
    REG_WRITE(ah, AR_CCCNT, 0);
    REG_WRITE(ah, AR_RFCNT, 0);
    REG_WRITE(ah, AR_RCCNT, 0);
    REG_WRITE(ah, AR_TFCNT, 0);
    REG_WRITE(ah, AR_MIBC, 0);

    listen = (int32_t)(cycles - rx - tx) / (CLOCK_RATE_2G * 1000);
    if (listen <= 0) {
        restart(ah);
        return 0;
    }
    a->listen_time += (uint32_t)listen;
    a->ofdm_errors = REG_READ(ah, AR_PHY_ERR_1);
    a->cck_errors = REG_READ(ah, AR_PHY_ERR_2);
    return 1;
}

/** Lower OFDM or CCK immunity, whichever's turn it is, as
 * ath9k_hw_ani_lower_immunity does. */
static void lower_immunity(struct ath9k *ah)
{
    struct ath9k_ani *a = &ah->ani;

    if (a->ofdm_level > 0 && (a->ofdms_turn || a->cck_level == 0)) {
        set_ofdm_level(ah, a->ofdm_level - 1u);
        return;
    }
    if (a->cck_level > 0)
        set_cck_level(ah, a->cck_level - 1u);
}

/**
 * Move the levels by the error rates, as ath9k_hw_ani_monitor does: once
 * the receiver has listened for 300 ms, both rates low lower one level, a
 * high OFDM rate raises OFDM's, a high CCK rate raises CCK's, and the
 * counts restart after any move.
 */
void ath9k_ani_monitor(struct ath9k *ah)
{
    struct ath9k_ani *a = &ah->ani;
    uint32_t ofdm_rate, cck_rate;

    if ((ah->options & ATH9K_OPT_NO_ANI) || !a->ready)
        return;
    if (!read_counters(ah))
        return;
    ofdm_rate = a->ofdm_errors * 1000 / a->listen_time;
    cck_rate = a->cck_errors * 1000 / a->listen_time;
    if (a->listen_time <= ANI_PERIOD)
        return;
    if (cck_rate < a->cck_trig_low && ofdm_rate < a->ofdm_trig_low) {
        lower_immunity(ah);
        a->ofdms_turn = !a->ofdms_turn;
    } else if (ofdm_rate > a->ofdm_trig_high) {
        if (a->ofdm_level < OFDM_MAX_LEVEL)
            set_ofdm_level(ah, a->ofdm_level + 1u);
        a->ofdms_turn = 0;
    } else if (cck_rate > a->cck_trig_high) {
        if (a->cck_level < CCK_MAX_LEVEL)
            set_cck_level(ah, a->cck_level + 1u);
        a->ofdms_turn = 1;
    } else
        return;
    restart(ah);
}
