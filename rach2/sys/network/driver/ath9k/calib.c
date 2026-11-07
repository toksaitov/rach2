/*
 * calib.c: the ath9k driver's calibrations: those of each reset, the
 * noise floor and its history, and the periodic work that keeps them.
 *
 * A reset calibrates the receiver's offsets and the transmitter's IQ
 * mismatch and carrier leak in one run of the baseband's AGC calibration,
 * whose TX IQ results the driver reads, cleans of outliers and writes
 * back. What a channel's calibration found is kept and written back at
 * the next reset on the same channel, and forgotten when the channel
 * changes. The noise floor is measured in the background, kept as a
 * history of five results per reading, and its median loaded back, so
 * that one noisy measurement does not move the receiver's threshold; a
 * load the chip does not take asks for a reset, since the radio is often
 * deaf after one. The periodic work runs the RX IQ calibration every
 * second until it is done and again every 20 minutes, the noise floor
 * every 30 seconds and noise immunity every second.
 *
 * The functions follow ath9k's ar9003_calib.c (ar9003_hw_init_cal_pcoem,
 * the TX IQ calibration's post-processing, outlier detection and reload,
 * the manual peak calibration, the carrier leak's post-processing, the
 * RX IQ calibration and ar9003_hw_calibrate), calib.c (the noise floor,
 * its history and the calibration list) and link.c (ath_ani_calibrate),
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
#include "../../../lib/kerrno.h"
#include "../../../lib/kstring.h"
#include "../../../klog.h"
#include "../../../timer.h"

/* The noise floor's bounds on 2.4 GHz in dBm (ar9003_phy.h): the
 * AR9300's from reg.h, and ath9k's AR9462 values, which hold for every
 * chip of its design. */
#define NF_MAX_2G         AR_PHY_CCA_MAX_GOOD_VAL_9300_2GHZ
#define NF_MIN_2G         AR_PHY_CCA_MIN_GOOD_VAL_9300_2GHZ
#define NF_NOM_2G         AR_PHY_CCA_NOM_VAL_9300_2GHZ
#define NF_MIN_2G_JUPITER (-127)
#define NF_NOM_2G_JUPITER (-127)

/* The results a new history takes whole before it takes medians
 * (calib.h's AR_PHY_CCA_FILTERWINDOW_LENGTH), how far above nominal the
 * floor may be before the noise level rises (ATH9K_NF_CAL_NOISE_THRESH),
 * and the noise level of a quiet channel, dBm (ATH_DEFAULT_NOISE_FLOOR). */
#define NF_FILTER_WINDOW 5
#define NF_NOISE_THRESH  6
#define NOISE_DEFAULT    (-95)

/* The value loaded into each reading after the median, so that the next
 * measurement is not capped by it, in dBm (ath9k_hw_loadnf). */
#define NF_RELEASE (-50)

/* The polls of the load's completion, 10 microseconds apart: 22.2 ms, the
 * longest frame of 802.11n (ath9k_hw_loadnf). */
#define NF_LOAD_POLLS 22200

/* The minimum CCA power of a chain's reading (ar9003_hw_do_getnf). */
#define MINCCA_PWR   0x1FF00000
#define MINCCA_PWR_S 20

/* The RX IQ calibration: the samples it takes, 2^(n + 1) per sample
 * (calib.h's MIN_CAL_SAMPLES and PER_MAX_LOG_COUNT). */
#define IQ_CAL_SAMPLES 1
#define IQ_CAL_LOG_MAX 10

/* The TX IQ calibration (ar9003_calib.c): the delay of its points, the
 * outlier bounds of magnitude and phase, and its passes kept per gain. */
#define DELPT         32
#define MAX_MAG_DELTA 11
#define MAX_PHS_DELTA 10
#define MAXIQCAL      3

/* The long calibration's period while the floor is not capped for
 * interference, in milliseconds (ath9k.h's ATH_LONG_CALINTERVAL_INT). */
#define LONG_CAL_INTF_MS 1000

/* The periodic work's period at its longest, in milliseconds. */
#define CAL_WORK_MAX_MS ATH9K_LONG_CAL_MS

/** The chains of the chip's row. */
static unsigned chainmask(const struct ath9k *ah)
{
    return ah->hw.chip->chainmask;
}

/** The number of chains in `mask`. */
static unsigned chain_count(unsigned mask)
{
    return (mask & 1) + ((mask >> 1) & 1) + ((mask >> 2) & 1);
}

/** The absolute value of `v`. */
static int iabs(int v)
{
    return v < 0 ? -v : v;
}

/** Set the receive, calibration and self-generated frames' chains, as
 * ar9003_hw_set_chain_masks does; phy.c has its twin. */
static void set_chain_masks(struct ath9k *ah, unsigned rx, unsigned tx)
{
    if (chainmask(ah) == 5)
        REG_SET_BIT(ah, AR_PHY_ANALOG_SWAP, AR_PHY_SWAP_ALT_CHAIN);
    REG_WRITE(ah, AR_PHY_RX_CHAINMASK, rx);
    REG_WRITE(ah, AR_PHY_CAL_CHAINMASK, rx);
    REG_WRITE(ah, AR_SELFGEN_MASK, tx);
}

/* ------------------------------------------------------------------ */
/* The noise floor (calib.c).                                          */
/* ------------------------------------------------------------------ */

/* The registers of the readings: the control channel's of three chains,
 * then the extension channel's (ar9003_hw_attach_phy_ops). */
static const uint32_t nf_regs[ATH9K_NF_READINGS] = {
    AR_PHY_CCA_0, AR_PHY_CCA_1, AR_PHY_CCA_2, AR_PHY_EXT_CCA, AR_PHY_EXT_CCA_1, AR_PHY_EXT_CCA_2};

/** The 2.4 GHz noise floor's maximum, minimum and nominal value for the
 * chip, as ar9003_hw_set_nf_limits sets them. */
static void nf_limits(const struct ath9k *ah, int *max, int *min, int *nominal)
{
    *max = NF_MAX_2G;
    if (ah->hw.chip->caps & ATH9K_CHIP_JUPITER) {
        *min = NF_MIN_2G_JUPITER;
        *nominal = NF_NOM_2G_JUPITER;
    } else {
        *min = NF_MIN_2G;
        *nominal = NF_NOM_2G;
    }
}

/** Whether reading `i` is in use: its chain is the chip's, and it is the
 * control channel's, the only one of an HT20 channel. */
static int nf_reading_used(const struct ath9k *ah, unsigned i)
{
    return i < AR9300_MAX_CHAINS && (chainmask(ah) & (1u << i));
}

/** The noise floor a reading of `chain` starts from: the EEPROM's at the
 * channel, or the nominal one (ath9k_hw_get_default_nf). */
static int nf_default(const struct ath9k *ah, unsigned chain)
{
    int max, min, nominal;

    if (ah->phy.nf_cal[chain])
        return ah->phy.nf_cal[chain];
    nf_limits(ah, &max, &min, &nominal);
    return nominal;
}

/** The noise level a channel with floor `nf` reports: the quiet level,
 * raised by however far the floor stands above its default
 * (ath9k_hw_getchan_noise). */
static int16_t chan_noise(const struct ath9k *ah, int nf)
{
    int noise = NOISE_DEFAULT, delta;

    if (nf) {
        delta = nf - NF_NOISE_THRESH - nf_default(ah, 0);
        if (delta > 0)
            noise += delta;
    }
    return (int16_t)noise;
}

/** Clear the history to the default floors, as
 * ath9k_init_nfcal_hist_buffer does. */
void ath9k_nf_init(struct ath9k *ah)
{
    unsigned i, j;

    for (i = 0; i < ATH9K_NF_READINGS; i++) {
        struct ath9k_nf_hist *h = &ah->cal.nf[i];

        h->index = 0;
        h->private_nf = (int16_t)nf_default(ah, i % AR9300_MAX_CHAINS);
        h->invalid_count = NF_FILTER_WINDOW;
        for (j = 0; j < ATH9K_NF_HISTORY; j++)
            h->buf[j] = h->private_nf;
    }
}

/** The median of a history's results (ath9k_hw_get_nf_hist_mid). */
static int16_t nf_median(const int16_t *buf)
{
    int16_t v[ATH9K_NF_HISTORY], t;
    unsigned i, j;

    for (i = 0; i < ATH9K_NF_HISTORY; i++)
        v[i] = buf[i];
    for (i = 1; i < ATH9K_NF_HISTORY; i++)
        for (j = i; j > 0 && v[j - 1] < v[j]; j--) {
            t = v[j];
            v[j] = v[j - 1];
            v[j - 1] = t;
        }
    return v[(ATH9K_NF_HISTORY - 1) >> 1];
}

/** Take a measurement's readings into the history, as
 * ath9k_hw_update_nfcal_hist_buffer does: a new history takes them whole,
 * then medians, capped at the maximum unless interference was seen. */
static void nf_hist_update(struct ath9k *ah, const int16_t *nf)
{
    struct ath9k_cal *cal = &ah->cal;
    int max, min, nominal, high = 0;
    unsigned i;

    nf_limits(ah, &max, &min, &nominal);
    for (i = 0; i < ATH9K_NF_READINGS; i++) {
        struct ath9k_nf_hist *h = &cal->nf[i];

        if (!nf_reading_used(ah, i))
            continue;
        h->buf[h->index] = nf[i];
        if (++h->index >= ATH9K_NF_HISTORY)
            h->index = 0;
        if (h->invalid_count > 0) {
            h->invalid_count--;
            h->private_nf = nf[i];
        } else
            h->private_nf = nf_median(h->buf);
        if (!h->private_nf)
            continue;
        if (h->private_nf > max) {
            high = 1;
            if (!(cal->flags & ATH9K_CAL_NF_INTF))
                h->private_nf = (int16_t)max;
        }
    }
    if (!high)
        cal->flags &= ~ATH9K_CAL_NF_INTF;
}

/** Correct each reading outside the chip's bounds: above the maximum to
 * it, below the minimum to nominal (ath9k_hw_nf_sanitize). */
static void nf_sanitize(const struct ath9k *ah, int16_t *nf)
{
    int max, min, nominal;
    unsigned i;

    nf_limits(ah, &max, &min, &nominal);
    for (i = 0; i < ATH9K_NF_READINGS; i++) {
        if (!nf[i])
            continue;
        if (nf[i] > max)
            nf[i] = (int16_t)max;
        else if (nf[i] < min)
            nf[i] = (int16_t)nominal;
    }
}

/**
 * Read a finished measurement into the history, as ath9k_hw_getnf does:
 * nothing while the chip's NF bit is still set; else each chain's
 * minimum CCA power, a signed 9-bit dBm value, corrected to the chip's
 * bounds. Updates cal.noise_floor and cal.noise. Returns 1 or 0.
 */
int ath9k_nf_read(struct ath9k *ah)
{
    int16_t nf[ATH9K_NF_READINGS];
    uint32_t v;
    unsigned i;

    if (!ah->phy.channel.frequency)
        return 0;
    if (REG_READ(ah, AR_PHY_AGC_CONTROL) & AR_PHY_AGC_CONTROL_NF)
        return 0;
    for (i = 0; i < ATH9K_NF_READINGS; i++) {
        nf[i] = 0;
        if (!nf_reading_used(ah, i))
            continue;
        v = MS(REG_READ(ah, nf_regs[i]), MINCCA_PWR);
        nf[i] = (int16_t)((v & 0x100) ? (int)v - 0x200 : (int)v);
    }
    nf_sanitize(ah, nf);
    ah->cal.flags &= ~ATH9K_CAL_NF_PENDING;
    nf_hist_update(ah, nf);
    ah->cal.noise_floor = ah->cal.nf[0].private_nf;
    ah->cal.noise = chan_noise(ah, ah->cal.noise_floor);
    return 1;
}

/**
 * Load the history's medians into the baseband, as ath9k_hw_loadnf does:
 * each reading's median into its register, a measurement in progress
 * stopped, the load started and waited for up to 22.2 ms, the stopped
 * measurement restarted, and each register released to -50 dBm so that
 * the next measurement is not capped by the median. Returns 0 or
 * -ETIMEDOUT, with the release not written, as ath9k leaves it.
 */
int ath9k_nf_load(struct ath9k *ah)
{
    uint32_t agc = REG_READ(ah, AR_PHY_AGC_CONTROL);
    unsigned i, j;

    for (i = 0; i < ATH9K_NF_READINGS; i++)
        if (nf_reading_used(ah, i))
            REG_RMW(ah, nf_regs[i], ((uint32_t)ah->cal.nf[i].private_nf << 1) & 0x1ff, 0x1ff);

    if (agc & AR_PHY_AGC_CONTROL_NF)
        REG_CLR_BIT(ah, AR_PHY_AGC_CONTROL, AR_PHY_AGC_CONTROL_NF);
    REG_CLR_BIT(ah, AR_PHY_AGC_CONTROL, AR_PHY_AGC_CONTROL_ENABLE_NF);
    REG_CLR_BIT(ah, AR_PHY_AGC_CONTROL, AR_PHY_AGC_CONTROL_NO_UPDATE_NF);
    REG_SET_BIT(ah, AR_PHY_AGC_CONTROL, AR_PHY_AGC_CONTROL_NF);

    for (j = 0; j < NF_LOAD_POLLS; j++) {
        if (!(REG_READ(ah, AR_PHY_AGC_CONTROL) & AR_PHY_AGC_CONTROL_NF))
            break;
        ATH9K_UDELAY(ah, 10);
    }

    if (agc & AR_PHY_AGC_CONTROL_NF) {
        if (agc & AR_PHY_AGC_CONTROL_ENABLE_NF)
            REG_SET_BIT(ah, AR_PHY_AGC_CONTROL, AR_PHY_AGC_CONTROL_ENABLE_NF);
        if (agc & AR_PHY_AGC_CONTROL_NO_UPDATE_NF)
            REG_SET_BIT(ah, AR_PHY_AGC_CONTROL, AR_PHY_AGC_CONTROL_NO_UPDATE_NF);
        REG_SET_BIT(ah, AR_PHY_AGC_CONTROL, AR_PHY_AGC_CONTROL_NF);
    }

    if (j == NF_LOAD_POLLS)
        return -ETIMEDOUT;

    for (i = 0; i < ATH9K_NF_READINGS; i++)
        if (nf_reading_used(ah, i))
            REG_RMW(ah, nf_regs[i], ((uint32_t)NF_RELEASE << 1) & 0x1ff, 0x1ff);
    return 0;
}

/** Start a measurement whose result the baseband takes itself when
 * `update`, or leaves to the driver's load, as ath9k_hw_start_nfcal
 * does. */
static void nf_start(struct ath9k *ah, int update)
{
    ah->cal.flags |= ATH9K_CAL_NF_PENDING;
    REG_SET_BIT(ah, AR_PHY_AGC_CONTROL, AR_PHY_AGC_CONTROL_ENABLE_NF);
    if (update)
        REG_CLR_BIT(ah, AR_PHY_AGC_CONTROL, AR_PHY_AGC_CONTROL_NO_UPDATE_NF);
    else
        REG_SET_BIT(ah, AR_PHY_AGC_CONTROL, AR_PHY_AGC_CONTROL_NO_UPDATE_NF);
    REG_SET_BIT(ah, AR_PHY_AGC_CONTROL, AR_PHY_AGC_CONTROL_NF);
}

/** Start a measurement whose result the baseband takes itself. */
void ath9k_nf_start(struct ath9k *ah)
{
    nf_start(ah, 1);
}

/** Read, load and restart, as ar9003_hw_calibrate's long calibration
 * does; a load the chip refused is -EIO. */
int ath9k_nf_update(struct ath9k *ah)
{
    if (!ath9k_nf_read(ah))
        return 0;
    if (ath9k_nf_load(ah) < 0)
        return -EIO;
    nf_start(ah, 0);
    return 1;
}

/**
 * React to stuck beacons, the likeliest sign of interference, as
 * ath9k_hw_bstuck_nfcal does: start a measurement the baseband takes
 * itself, or read the one that finished, and stop capping the floor at its
 * maximum until a measurement finds it normal again.
 */
void ath9k_nf_bstuck(struct ath9k *ah)
{
    if (!(ah->cal.flags & ATH9K_CAL_NF_PENDING))
        nf_start(ah, 1);
    else if (!(REG_READ(ah, AR_PHY_AGC_CONTROL) & AR_PHY_AGC_CONTROL_NF))
        (void)ath9k_nf_read(ah);
    ah->cal.flags |= ATH9K_CAL_NF_INTF;
}

/** Forget the calibration data unless it is `chan`'s, and give the noise
 * floor its defaults again (ath9k_hw_reset). */
void ath9k_cal_set_channel(struct ath9k *ah, const struct wlan_channel *chan)
{
    struct ath9k_cal *cal = &ah->cal;

    if (cal->freq != chan->frequency) {
        cal->freq = chan->frequency;
        cal->flags = 0;
        cal->iq_valid = 0;
        k_memset(cal->tx_corr, 0, sizeof(cal->tx_corr));
        k_memset(cal->num_measures, 0, sizeof(cal->num_measures));
        k_memset(cal->tx_clcal, 0, sizeof(cal->tx_clcal));
        cal->noise_floor = 0;
        ath9k_nf_init(ah);
    }
    cal->noise = chan_noise(ah, cal->noise_floor);
}

/* ------------------------------------------------------------------ */
/* The RX IQ calibration (ar9003_calib.c, calib.c).                    */
/* ------------------------------------------------------------------ */

/** Start a sample of the RX IQ calibration, as ar9003_hw_setup_calibration
 * does. */
static void iq_setup(struct ath9k *ah)
{
    REG_RMW_FIELD(ah, AR_PHY_TIMING4, AR_PHY_TIMING4_IQCAL_LOG_COUNT_MAX, IQ_CAL_LOG_MAX);
    REG_WRITE(ah, AR_PHY_CALMODE, AR_PHY_CALMODE_IQ);
    REG_SET_BIT(ah, AR_PHY_TIMING4, AR_PHY_TIMING4_DO_CAL);
}

/** Start the RX IQ calibration over, its sums cleared, as
 * ath9k_hw_reset_calibration does. */
static void iq_reset(struct ath9k *ah)
{
    struct ath9k_cal *cal = &ah->cal;

    iq_setup(ah);
    cal->iq_state = ATH9K_CAL_RUNNING;
    k_memset(cal->meas_i, 0, sizeof(cal->meas_i));
    k_memset(cal->meas_q, 0, sizeof(cal->meas_q));
    k_memset(cal->meas_iq, 0, sizeof(cal->meas_iq));
    cal->samples = 0;
}

/** Add a sample's measurements of each chain, as ar9003_hw_iqcal_collect
 * does. */
static void iq_collect(struct ath9k *ah)
{
    unsigned i;

    for (i = 0; i < AR9300_MAX_CHAINS; i++) {
        if (!(chainmask(ah) & (1u << i)))
            continue;
        ah->cal.meas_i[i] += REG_READ(ah, AR_PHY_CAL_MEAS_0(i));
        ah->cal.meas_q[i] += REG_READ(ah, AR_PHY_CAL_MEAS_1(i));
        ah->cal.meas_iq[i] += (int32_t)REG_READ(ah, AR_PHY_CAL_MEAS_2(i));
    }
}

/**
 * Compute each chain's RX IQ correction from the sums and write it, as
 * ar9003_hw_iqcalibrate does, and enable the correction. On a chip with
 * ATH9K_CHIP_IQ_BOUND a coefficient at its bound skips the update, the
 * enable among it, since the AR9565 then drops its best receive rate to
 * MCS 4 (ath9k 6f37ff96d3bd). ath9k compares after masking to seven
 * bits, which catches only +63; the comparison here is before it, and
 * catches both bounds the commit names.
 */
static void iq_calibrate(struct ath9k *ah, unsigned chains)
{
    static const uint32_t corr[AR9300_MAX_CHAINS] = {
        AR_PHY_RX_IQCAL_CORR_B0, AR_PHY_RX_IQCAL_CORR_B1, AR_PHY_RX_IQCAL_CORR_B2};
    uint32_t pi, pq, iq, iden, qden;
    int32_t icoff, qcoff;
    int neg;
    unsigned i;

    for (i = 0; i < chains && i < AR9300_MAX_CHAINS; i++) {
        pi = ah->cal.meas_i[i];
        pq = ah->cal.meas_q[i];
        iq = (uint32_t)ah->cal.meas_iq[i];
        neg = 0;
        if (iq > 0x80000000u) {
            iq = (0xffffffffu - iq) + 1;
            neg = 1;
        }
        iden = (pi / 2 + pq / 2) / 256;
        qden = pq / 64;
        if (iden == 0 || qden == 0)
            continue;
        icoff = (int32_t)(iq / iden);
        qcoff = (int32_t)(pi / qden) - 64;
        if (icoff >= 63)
            icoff = 63;
        else if (icoff <= -63)
            icoff = -63;
        if (!neg)
            icoff = -icoff;
        if (qcoff >= 63)
            qcoff = 63;
        else if (qcoff <= -63)
            qcoff = -63;
        if ((ah->hw.chip->caps & ATH9K_CHIP_IQ_BOUND) && (iabs(icoff) == 63 || iabs(qcoff) == 63))
            return;
        REG_RMW_FIELD(ah, corr[i], AR_PHY_RX_IQCAL_CORR_IQCORR_Q_I_COFF, (uint32_t)icoff & 0x7f);
        REG_RMW_FIELD(ah, corr[i], AR_PHY_RX_IQCAL_CORR_IQCORR_Q_Q_COFF, (uint32_t)qcoff & 0x7f);
    }
    REG_SET_BIT(ah, AR_PHY_RX_IQCAL_CORR_B0, AR_PHY_RX_IQCAL_CORR_IQCORR_ENABLE);
}

/** Advance the RX IQ calibration, as ar9003_hw_per_calibration does.
 * Returns 1 when it finished now. */
static int iq_step(struct ath9k *ah)
{
    struct ath9k_cal *cal = &ah->cal;

    if (cal->iq_state == ATH9K_CAL_RUNNING) {
        if (REG_READ(ah, AR_PHY_TIMING4) & AR_PHY_TIMING4_DO_CAL)
            return 0;
        iq_collect(ah);
        cal->samples++;
        if (cal->samples >= IQ_CAL_SAMPLES) {
            iq_calibrate(ah, chain_count(chainmask(ah)));
            cal->iq_valid = 1;
            cal->iq_state = ATH9K_CAL_DONE;
            return 1;
        }
        iq_setup(ah);
    } else if (!cal->iq_valid)
        iq_reset(ah);
    return 0;
}

/** Ask for the RX IQ calibration again once it is done, as
 * ath9k_hw_reset_calvalid does. Returns 1 when it was not done, so that
 * nothing changes, or 0 when it will run again. */
static int iq_restart(struct ath9k *ah)
{
    if (ah->cal.iq_state != ATH9K_CAL_DONE)
        return 1;
    ah->cal.iq_valid = 0;
    ah->cal.iq_state = ATH9K_CAL_WAITING;
    return 0;
}

/**
 * The periodic calibration, as ar9003_hw_calibrate does: a step of the RX
 * IQ calibration and, on a long calibration, the noise floor. Returns 1
 * when the RX IQ calibration is done, 0 while it runs, or -EIO when the
 * noise floor's load failed.
 */
static int calibrate(struct ath9k *ah, int longcal)
{
    struct ath9k_cal *cal = &ah->cal;
    int done = 1;

    if (cal->iq_state == ATH9K_CAL_RUNNING || cal->iq_state == ATH9K_CAL_WAITING) {
        done = iq_step(ah);
        if (done && cal->iq_state == ATH9K_CAL_WAITING) {
            done = 0;
            iq_reset(ah);
        }
    }
    if (longcal && ath9k_nf_update(ah) < 0)
        return -EIO;
    return done;
}

/* ------------------------------------------------------------------ */
/* The TX IQ calibration (ar9003_calib.c).                             */
/* ------------------------------------------------------------------ */

/* The magnitude and phase coefficients of each chain, gain and pass. */
struct coeff {
    int mag[AR9300_MAX_CHAINS][ATH9K_IQCAL_GAINS][MAXIQCAL];
    int phs[AR9300_MAX_CHAINS][ATH9K_IQCAL_GAINS][MAXIQCAL];
    int iqc[2];
};

/** `v` shifted right by `n` with its sign kept, rounding down, as the
 * compilers ath9k is built with shift a negative value; C leaves that
 * shift to the implementation. */
static int32_t asr(int32_t v, unsigned n)
{
    return v >= 0 ? v >> n : -((-(v + 1)) >> n) - 1;
}

/* One pass of the loopback, without or with the analog phase shift: the
 * normalized sine and cosine of its angle, and the magnitude and phase of
 * its first measurement, each in units of 1 << 15. */
struct iq_pass {
    int32_t sin, cos;
    int32_t mag, phs;
};

/** Solve the 4x4 linear equation of the loopback IQ calibration from the
 * passes without (`a0`) and with (`a1`) the phase shift, as
 * ar9003_hw_solve_iq_cal does. Returns 0 when it is singular. */
static int solve_iq_cal(const struct iq_pass *a0, const struct iq_pass *a1, int32_t *solved)
{
    const int32_t shift = 1 << 15;
    int32_t f1 = a0->cos - a1->cos, f3 = a0->sin - a1->sin, f2;
    int32_t mag_tx, phs_tx;

    f2 = (asr(f1, 3) * asr(f1, 3) + asr(f3, 3) * asr(f3, 3)) >> 9;
    if (!f2)
        return 0;
    mag_tx = (f1 * (a0->mag - a1->mag) + f3 * (a0->phs - a1->phs)) / f2;
    phs_tx = (f3 * (-a0->mag + a1->mag) + f1 * (a0->phs - a1->phs)) / f2;
    solved[0] = mag_tx;
    solved[1] = phs_tx;
    solved[2] = a0->mag - (a0->cos * mag_tx + a0->sin * phs_tx) / shift;
    solved[3] = a0->phs + (a0->sin * mag_tx - a0->cos * phs_tx) / shift;
    return 1;
}

/** An approximate magnitude of (re, im), as ar9003_hw_find_mag_approx
 * computes it. */
static int32_t mag_approx(int32_t re, int32_t im)
{
    int32_t a = iabs(re), b = iabs(im), max, min;

    max = a > b ? a : b;
    min = a > b ? b : a;
    return max - max / 32 + min / 8 + min / 4;
}

/** One gain's correction word from its magnitude and phase corrections,
 * in units of 1 << 15: each scaled and held to -63..63, the magnitude from
 * bit 7 up and the phase in the 7 bits below, as ar9003_hw_calc_iq_corr packs
 * them. */
static int iq_corr_word(int32_t mag_corr, int32_t phs_corr)
{
    const int32_t scale = 1 << 15;
    int32_t qq = mag_corr * 128 / scale, qi = phs_corr * 256 / scale;

    qi = qi < -63 ? -63 : qi > 63 ? 63 : qi;
    qq = qq < -63 ? -63 : qq > 63 ? 63 : qq;
    return (qq * 128) + (0x7f & qi);
}

/** A 12-bit two's complement field as a signed value. */
static int32_t s12(uint32_t v)
{
    v &= 0xfff;
    return v > 0x800 ? (int32_t)v - 0x1000 : (int32_t)v;
}

/**
 * Compute one gain's TX and RX IQ correction from the chip's six result
 * words, as ar9003_hw_calc_iq_corr does. Returns 0 when the results are
 * unusable. Its arithmetic is ath9k's, in 32 bits.
 */
static int calc_iq_corr(const uint32_t *res, int *iqc)
{
    const int32_t scale = 1 << 15, delpt_shift = 1 << 8;
    int32_t m_a0_d0, p_a0_d0, c_a0_d0, m_a0_d1, p_a0_d1, c_a0_d1;
    int32_t m_a1_d0, p_a1_d0, c_a1_d0, m_a1_d1, p_a1_d1, c_a1_d1;
    int32_t mag_a0_d0, mag_a1_d0, mag_a0_d1, mag_a1_d1;
    int32_t phs_a0_d0, phs_a1_d0, phs_a0_d1, phs_a1_d1;
    int32_t mag1, mag2, solved[4];
    struct iq_pass a0, a1;

    m_a0_d0 = s12(res[0]);
    p_a0_d0 = s12(res[0] >> 12);
    c_a0_d0 = s12(((res[0] >> 24) & 0xff) + ((res[1] & 0xf) << 8));
    m_a0_d1 = s12(res[1] >> 4);
    p_a0_d1 = (int32_t)(res[2] & 0xfff);
    c_a0_d1 = s12(res[2] >> 12);
    m_a1_d0 = s12(((res[2] >> 24) & 0xff) + ((res[3] & 0xf) << 8));
    p_a1_d0 = s12(res[3] >> 4);
    c_a1_d0 = s12(res[4]);
    m_a1_d1 = s12(res[4] >> 12);
    p_a1_d1 = s12(((res[4] >> 24) & 0xff) + ((res[5] & 0xf) << 8));
    c_a1_d1 = s12(res[5] >> 4);

    if (p_a0_d0 == 0 || p_a0_d1 == 0 || p_a1_d0 == 0 || p_a1_d1 == 0)
        return 0;
    if (p_a0_d0 < 1024 || p_a0_d0 > 2047 || p_a1_d0 < 0 || p_a1_d1 < 0 || p_a0_d0 <= m_a0_d0 ||
        p_a0_d0 <= c_a0_d0 || p_a0_d1 <= m_a0_d1 || p_a0_d1 <= c_a0_d1 || p_a1_d0 <= m_a1_d0 ||
        p_a1_d0 <= c_a1_d0 || p_a1_d1 <= m_a1_d1 || p_a1_d1 <= c_a1_d1)
        return 0;

    mag_a0_d0 = (m_a0_d0 * scale) / p_a0_d0;
    phs_a0_d0 = (c_a0_d0 * scale) / p_a0_d0;
    mag_a0_d1 = (m_a0_d1 * scale) / p_a0_d1;
    phs_a0_d1 = (c_a0_d1 * scale) / p_a0_d1;
    mag_a1_d0 = (m_a1_d0 * scale) / p_a1_d0;
    phs_a1_d0 = (c_a1_d0 * scale) / p_a1_d0;
    mag_a1_d1 = (m_a1_d1 * scale) / p_a1_d1;
    phs_a1_d1 = (c_a1_d1 * scale) / p_a1_d1;

    /* Without and with the analog phase shift. */
    a0.sin = ((mag_a0_d0 - mag_a0_d1) * delpt_shift) / DELPT;
    a0.cos = ((phs_a0_d1 - phs_a0_d0) * delpt_shift) / DELPT;
    a1.sin = ((mag_a1_d0 - mag_a1_d1) * delpt_shift) / DELPT;
    a1.cos = ((phs_a1_d1 - phs_a1_d0) * delpt_shift) / DELPT;

    mag1 = mag_approx(a0.cos, a0.sin);
    mag2 = mag_approx(a1.cos, a1.sin);
    if (mag1 == 0 || mag2 == 0)
        return 0;
    a0.sin = a0.sin * scale / mag1;
    a0.cos = a0.cos * scale / mag1;
    a1.sin = a1.sin * scale / mag2;
    a1.cos = a1.cos * scale / mag2;
    a0.mag = mag_a0_d0;
    a0.phs = phs_a0_d0;
    a1.mag = mag_a1_d0;
    a1.phs = phs_a1_d0;

    if (!solve_iq_cal(&a0, &a1, solved))
        return 0;

    if (scale == solved[0])
        return 0;
    iqc[0] = iq_corr_word((solved[0] * scale) / (scale - solved[0]), -solved[1]);
    if (-solved[2] == scale)
        return 0;
    iqc[1] = iq_corr_word((-solved[2] * scale) / (scale + solved[2]), -solved[3]);
    return 1;
}

/** Replace the worst of `n` gains' first-pass coefficients with the
 * others' mean when they spread by more than `max_delta`, as
 * ar9003_hw_detect_outlier does. */
static void detect_outlier(int mp[][MAXIQCAL], int n, int max_delta)
{
    int max = -64, max_idx = 0, min = 63, min_idx = 0;
    int avg = 0, count = 0, i, outlier;

    for (i = 0; i < n; i++) {
        if (mp[i][0] > max) {
            max = mp[i][0];
            max_idx = i;
        } else if (mp[i][0] < min) {
            min = mp[i][0];
            min_idx = i;
        }
    }
    for (i = 0; i < n; i++) {
        if (iabs(mp[i][0]) < iabs(max) || iabs(mp[i][0]) < iabs(min)) {
            avg += mp[i][0];
            count++;
        }
    }
    if (count)
        avg /= count;
    else
        avg = mp[n - 1][0];
    if (iabs(max - min) > max_delta) {
        outlier = iabs(max - avg) > iabs(min - avg) ? max_idx : min_idx;
        mp[outlier][0] = avg;
    }
}

/** The register of chain `chain`'s TX IQ correction of gain `gain`: two
 * gains share a register, in its 00 and 01 fields. */
static uint32_t tx_corr_reg(unsigned chain, unsigned gain)
{
    if (chain == 1)
        return AR_PHY_TX_IQCAL_CORR_COEFF_B1(gain / 2);
    if (chain == 2)
        return AR_PHY_TX_IQCAL_CORR_COEFF_B2(gain / 2);
    return AR_PHY_TX_IQCAL_CORR_COEFF_B0(gain / 2);
}

/** Write chain `chain`'s correction `value` of gain `gain`. */
static void tx_corr_write(struct ath9k *ah, unsigned chain, unsigned gain, uint32_t value)
{
    if (gain % 2 == 0)
        REG_RMW_FIELD(ah, tx_corr_reg(chain, gain), AR_PHY_TX_IQCAL_CORR_COEFF_00_COEFF_TABLE,
                      value);
    else
        REG_RMW_FIELD(ah, tx_corr_reg(chain, gain), AR_PHY_TX_IQCAL_CORR_COEFF_01_COEFF_TABLE,
                      value);
}

/** Turn the TX IQ correction and the loopback's RX IQ correction on. */
static void tx_corr_enable(struct ath9k *ah)
{
    REG_RMW_FIELD(ah, AR_PHY_TX_IQCAL_CONTROL_3, AR_PHY_TX_IQCAL_CONTROL_3_IQCORR_EN, 0x1);
    REG_RMW_FIELD(ah, AR_PHY_RX_IQCAL_CORR_B0, AR_PHY_RX_IQCAL_CORR_B0_LOOPBACK_IQCORR_EN, 0x1);
}

/** Write each chain's coefficients, outliers replaced, keep them for the
 * channel and enable the correction, as
 * ar9003_hw_tx_iq_cal_outlier_detection does. */
static void tx_iq_outliers(struct ath9k *ah, struct coeff *c, int reusable)
{
    unsigned i;
    int im, n;

    for (i = 0; i < AR9300_MAX_CHAINS; i++) {
        if (!(chainmask(ah) & (1u << i)))
            continue;
        n = (int)REG_READ_FIELD(ah, AR_PHY_TX_IQCAL_STATUS_B0, AR_PHY_CALIBRATED_GAINS_0);
        if (n > ATH9K_IQCAL_GAINS)
            n = ATH9K_IQCAL_GAINS;
        if (n > 1) {
            detect_outlier(c->mag[i], n, MAX_MAG_DELTA);
            detect_outlier(c->phs[i], n, MAX_PHS_DELTA);
        }
        for (im = 0; im < n; im++) {
            c->iqc[0] = (c->phs[i][im][0] & 0x7f) | ((c->mag[i][im][0] & 0x7f) << 7);
            tx_corr_write(ah, i, (unsigned)im, (uint32_t)c->iqc[0]);
            ah->cal.tx_corr[im][i] = (uint16_t)c->iqc[0];
        }
        ah->cal.num_measures[i] = (uint8_t)n;
    }
    tx_corr_enable(ah);
    if (reusable)
        ah->cal.flags |= ATH9K_CAL_TXIQ_DONE;
    else
        ah->cal.flags &= ~ATH9K_CAL_TXIQ_DONE;
}

/**
 * Read the TX IQ calibration's results of each chain and gain from the
 * channel information memory, compute their corrections and write them,
 * as ar9003_hw_tx_iq_cal_post_proc does for one pass. A gain the chip
 * reports failed, or whose results are unusable, leaves every correction
 * as it was.
 */
static void tx_iq_post_proc(struct ath9k *ah, int reusable)
{
    static const uint32_t status[AR9300_MAX_CHAINS] = {
        AR_PHY_TX_IQCAL_STATUS_B0, AR_PHY_TX_IQCAL_STATUS_B1, AR_PHY_TX_IQCAL_STATUS_B2};
    static const uint32_t tab[AR9300_MAX_CHAINS] = {AR_PHY_CHAN_INFO_TAB_0, AR_PHY_CHAN_INFO_TAB_1,
                                                    AR_PHY_CHAN_INFO_TAB_2};
    static struct coeff c; /* 600 bytes, too many for the stack; one card */
    uint32_t res[6];
    unsigned i, j;
    int im, n;

    for (i = 0; i < AR9300_MAX_CHAINS; i++) {
        if (!(chainmask(ah) & (1u << i)))
            continue;
        n = (int)REG_READ_FIELD(ah, AR_PHY_TX_IQCAL_STATUS_B0, AR_PHY_CALIBRATED_GAINS_0);
        if (n > ATH9K_IQCAL_GAINS)
            n = ATH9K_IQCAL_GAINS;
        for (im = 0; im < n; im++) {
            if (REG_READ(ah, status[i]) & AR_PHY_TX_IQCAL_STATUS_FAILED)
                return;
            for (j = 0; j < 3; j++) {
                uint32_t offset = 4 * (3 * (uint32_t)im + j);

                REG_RMW_FIELD(ah, AR_PHY_CHAN_INFO_MEMORY, AR_PHY_CHAN_INFO_TAB_S2_READ, 0);
                res[2 * j] = REG_READ(ah, tab[i] + offset);
                REG_RMW_FIELD(ah, AR_PHY_CHAN_INFO_MEMORY, AR_PHY_CHAN_INFO_TAB_S2_READ, 1);
                res[2 * j + 1] = 0xffff & REG_READ(ah, tab[i] + offset);
            }
            if (!calc_iq_corr(res, c.iqc))
                return;
            c.phs[i][im][0] = c.iqc[0] & 0x7f;
            c.mag[i][im][0] = (c.iqc[0] >> 7) & 0x7f;
            if (c.mag[i][im][0] > 63)
                c.mag[i][im][0] -= 128;
            if (c.phs[i][im][0] > 63)
                c.phs[i][im][0] -= 128;
        }
    }
    tx_iq_outliers(ah, &c, reusable);
}

/** Write back the channel's TX IQ corrections, as
 * ar9003_hw_tx_iq_cal_reload does. */
static void tx_iq_reload(struct ath9k *ah)
{
    unsigned i, im;

    for (i = 0; i < AR9300_MAX_CHAINS; i++) {
        if (!(chainmask(ah) & (1u << i)))
            continue;
        for (im = 0; im < ah->cal.num_measures[i]; im++)
            tx_corr_write(ah, i, im, ah->cal.tx_corr[im][i]);
    }
    tx_corr_enable(ah);
}

/* ------------------------------------------------------------------ */
/* The reset-time calibrations (ar9003_calib.c).                       */
/* ------------------------------------------------------------------ */

/**
 * Calibrate chain `chain`'s 2.4 GHz peak detector by hand, as
 * ar9003_hw_manual_peak_cal does on a chip made for PC makers: LNA and
 * receive off, the AGC's calibration DAC found bit by bit from the top
 * until the AGC's output turns, then the overrides released.
 */
static void manual_peak_cal(struct ath9k *ah, unsigned chain)
{
    int i, total = 0, test, agc_out;

    REG_RMW_FIELD(ah, AR_PHY_65NM_RXRF_GAINSTAGES(chain), AR_PHY_65NM_RXRF_GAINSTAGES_RX_OVERRIDE,
                  0x1);
    REG_RMW_FIELD(ah, AR_PHY_65NM_RXRF_GAINSTAGES(chain), AR_PHY_65NM_RXRF_GAINSTAGES_LNAON_CALDC,
                  0x0);
    if (ah->hw.chip->caps & ATH9K_CHIP_PCOEM)
        REG_RMW_FIELD(ah, AR_PHY_65NM_RXRF_GAINSTAGES(chain),
                      AR_PHY_65NM_RXRF_GAINSTAGES_LNA2G_GAIN_OVR, 0x0);
    REG_RMW_FIELD(ah, AR_PHY_65NM_RXTX2(chain), AR_PHY_65NM_RXTX2_RXON_OVR, 0x1);
    REG_RMW_FIELD(ah, AR_PHY_65NM_RXTX2(chain), AR_PHY_65NM_RXTX2_RXON, 0x0);
    REG_RMW_FIELD(ah, AR_PHY_65NM_RXRF_AGC(chain), AR_PHY_65NM_RXRF_AGC_AGC_OVERRIDE, 0x1);
    REG_RMW_FIELD(ah, AR_PHY_65NM_RXRF_AGC(chain), AR_PHY_65NM_RXRF_AGC_AGC_ON_OVR, 0x1);
    REG_RMW_FIELD(ah, AR_PHY_65NM_RXRF_AGC(chain), AR_PHY_65NM_RXRF_AGC_AGC_CAL_OVR, 0x1);
    REG_RMW_FIELD(ah, AR_PHY_65NM_RXRF_AGC(chain), AR_PHY_65NM_RXRF_AGC_AGC2G_DBDAC_OVR, 0);

    for (i = 6; i > 0; i--) {
        test = total + (1 << (i - 1));
        REG_RMW_FIELD(ah, AR_PHY_65NM_RXRF_AGC(chain), AR_PHY_65NM_RXRF_AGC_AGC2G_CALDAC_OVR, test);
        ATH9K_UDELAY(ah, 100);
        agc_out =
            (int)REG_READ_FIELD(ah, AR_PHY_65NM_RXRF_AGC(chain), AR_PHY_65NM_RXRF_AGC_AGC_OUT);
        total += (agc_out ? 0 : 1) << (i - 1);
    }
    REG_RMW_FIELD(ah, AR_PHY_65NM_RXRF_AGC(chain), AR_PHY_65NM_RXRF_AGC_AGC2G_CALDAC_OVR, total);

    REG_RMW_FIELD(ah, AR_PHY_65NM_RXRF_GAINSTAGES(chain), AR_PHY_65NM_RXRF_GAINSTAGES_RX_OVERRIDE,
                  0);
    REG_RMW_FIELD(ah, AR_PHY_65NM_RXTX2(chain), AR_PHY_65NM_RXTX2_RXON_OVR, 0);
    REG_RMW_FIELD(ah, AR_PHY_65NM_RXRF_AGC(chain), AR_PHY_65NM_RXRF_AGC_AGC_CAL_OVR, 0);
}

/** The register of entry `j` of chain `chain`'s carrier-leak table. */
static uint32_t cl_reg(unsigned chain, unsigned j)
{
    static const uint32_t tab[AR9300_MAX_CHAINS] = {AR_PHY_CL_TAB_0, AR_PHY_CL_TAB_1,
                                                    AR_PHY_CL_TAB_2};

    return tab[chain] + 4 * j;
}

/** Write back the channel's carrier-leak table, or keep the one the
 * calibration just found, as ar9003_hw_cl_cal_post_proc does. */
static void cl_post_proc(struct ath9k *ah, int reusable)
{
    struct ath9k_cal *cal = &ah->cal;
    unsigned i, j;
    int done;

    if (!(cal->enabled & ATH9K_CAL_ENABLE_TXCL))
        return;
    done = (REG_READ(ah, AR_PHY_AGC_CONTROL) & AR_PHY_AGC_CONTROL_CLC_SUCCESS) != 0;
    if (cal->flags & ATH9K_CAL_TXCL_DONE) {
        for (i = 0; i < AR9300_MAX_CHAINS; i++)
            if (chainmask(ah) & (1u << i))
                for (j = 0; j < ATH9K_CL_ENTRIES; j++)
                    REG_WRITE(ah, cl_reg(i, j), cal->tx_clcal[i][j]);
    } else if (reusable && done) {
        for (i = 0; i < AR9300_MAX_CHAINS; i++)
            if (chainmask(ah) & (1u << i))
                for (j = 0; j < ATH9K_CL_ENTRIES; j++)
                    cal->tx_clcal[i][j] = REG_READ(ah, cl_reg(i, j));
        cal->flags |= ATH9K_CAL_TXCL_DONE;
    }
}

/**
 * Whether ath9k would reset with calibration data, which it keeps for the
 * channel a station has joined and gives no reset at start or off
 * channel: here, while the chip has a BSSID. Without it TX IQ calibration
 * does not run, as the trace of Linux's start and scan shows.
 */
static int has_caldata(const struct ath9k *ah)
{
    const uint8_t *b = ah->mac.bssid;

    return (b[0] | b[1] | b[2] | b[3] | b[4] | b[5]) != 0;
}

/**
 * Run the reset-time calibrations, as ar9003_hw_init_cal_pcoem does on a chip
 * without RTT and with coexistence off: every chain calibrates; the carrier
 * leak runs, since a full reset forgets its table, and the TX IQ calibration
 * runs as part of the AGC calibration when ath9k would have calibration data
 * (has_caldata); with the carrier leak on, the baseband restarts with the
 * longest receive delay around the AGC calibration, which runs at every reset,
 * since only ath9k's fast channel change may skip it; then the manual peak
 * calibration, the TX IQ and carrier-leak results, and the RX IQ calibration
 * started over. The AR9565 1.1's RTT, which keeps the calibration across
 * channels, is not used: the G50-70's chip is a 1.0.1, and without RTT the 1.1
 * calibrates as the 1.0 does. A chip not made for PC makers takes ath9k's
 * ar9003_hw_init_cal_soc, which no row needs.
 */
int ath9k_cal_init(struct ath9k *ah, const struct wlan_channel *chan)
{
    struct ath9k_cal *cal = &ah->cal;
    uint32_t rx_delay = 0;
    int txiq_done = 0, ok;
    unsigned i;

    (void)chan;
    /* A full reset calibrates again, as ath9k_hw_reset clears both before
     * ath9k_hw_init_cal. */
    cal->flags &= ~(ATH9K_CAL_TXIQ_DONE | ATH9K_CAL_TXCL_DONE);
    set_chain_masks(ah, chainmask(ah), chainmask(ah));

    if (cal->enabled & ATH9K_CAL_ENABLE_TXCL) {
        if (cal->flags & ATH9K_CAL_TXCL_DONE)
            REG_CLR_BIT(ah, AR_PHY_CL_CAL_CTL, AR_PHY_CL_CAL_ENABLE);
        else
            REG_SET_BIT(ah, AR_PHY_CL_CAL_CTL, AR_PHY_CL_CAL_ENABLE);
    }

    if (cal->enabled & ATH9K_CAL_ENABLE_TXIQ) {
        REG_RMW_FIELD(ah, AR_PHY_TX_IQCAL_CONTROL_1,
                      AR_PHY_TX_IQCAL_CONTROL_1_IQCORR_I_Q_COFF_DELPT, DELPT);
        if (cal->enabled & ATH9K_CAL_ENABLE_TXIQ_ON_AGC) {
            if (has_caldata(ah))
                REG_SET_BIT(ah, AR_PHY_TX_IQCAL_CONTROL_0,
                            AR_PHY_TX_IQCAL_CONTROL_0_ENABLE_TXIQ_CAL);
            else
                REG_CLR_BIT(ah, AR_PHY_TX_IQCAL_CONTROL_0,
                            AR_PHY_TX_IQCAL_CONTROL_0_ENABLE_TXIQ_CAL);
            txiq_done = 1;
        }
    }

    if (REG_READ(ah, AR_PHY_CL_CAL_CTL) & AR_PHY_CL_CAL_ENABLE) {
        rx_delay = REG_READ(ah, AR_PHY_RX_DELAY);
        REG_WRITE(ah, AR_PHY_ACTIVE, AR_PHY_ACTIVE_DIS);
        ATH9K_UDELAY(ah, 5);
        REG_WRITE(ah, AR_PHY_RX_DELAY, AR_PHY_RX_DELAY_DELAY);
        REG_WRITE(ah, AR_PHY_ACTIVE, AR_PHY_ACTIVE_EN);
    }

    REG_WRITE(ah, AR_PHY_AGC_CONTROL, REG_READ(ah, AR_PHY_AGC_CONTROL) | AR_PHY_AGC_CONTROL_CAL);
    ok = ath9k_wait(ah, AR_PHY_AGC_CONTROL, AR_PHY_AGC_CONTROL_CAL, 0, ATH9K_WAIT_TIMEOUT) == 0;
    for (i = 0; i < AR9300_MAX_CHAINS; i++)
        if (chainmask(ah) & (1u << i))
            manual_peak_cal(ah, i);

    if (REG_READ(ah, AR_PHY_CL_CAL_CTL) & AR_PHY_CL_CAL_ENABLE) {
        REG_WRITE(ah, AR_PHY_RX_DELAY, rx_delay);
        ATH9K_UDELAY(ah, 5);
    }

    if (!ok) {
        klog(KLOG_WARN, "ath9k", "cal: offset calibration did not finish in %u ms",
             (unsigned)(ATH9K_WAIT_TIMEOUT / 1000));
        return -EIO;
    }

    if (txiq_done)
        tx_iq_post_proc(ah, 1);
    else if (cal->flags & ATH9K_CAL_TXIQ_DONE)
        tx_iq_reload(ah);
    cl_post_proc(ah, 1);

    set_chain_masks(ah, chainmask(ah), chainmask(ah));

    /* The calibration list, as ar9003_hw_init_cal_common makes it. */
    cal->iq_state = ATH9K_CAL_WAITING;
    iq_reset(ah);
    cal->iq_valid = 0;
    return 0;
}

/* ------------------------------------------------------------------ */
/* The periodic work (link.c).                                         */
/* ------------------------------------------------------------------ */

/** Queue the record of the periodic work not queued last, for `due`. */
static void cal_queue(struct ath9k_cal *cal, uint64_t due)
{
    struct ath9k_cal_work *w = &cal->work[cal->next_work];

    cal->next_work ^= 1;
    (void)wlan_queue_work(&w->work, due);
}

/** Whether `period` milliseconds have passed from `since` to `now`. */
static int elapsed(uint64_t now, uint64_t since, uint64_t period)
{
    return now - since >= period;
}

/**
 * The periodic calibration, as ath_ani_calibrate does: a long calibration
 * every 30 seconds, or every second while interference keeps the floor
 * uncapped; the short calibration every second until it is done, and its
 * restart 20 minutes after; ANI every second; and the work queued again
 * after the shortest period still due. The thermometer's latest reading
 * is kept in cal.temp. A failed calibration asks recover.c for a reset,
 * which starts the work again, and queues nothing. While the chip's
 * interrupts are off, from a reset or a retune that has not finished or
 * one that failed, the chip may be held in reset, so the work touches no
 * register and looks again a period later, as ath_ani_calibrate skips a
 * chip that is not awake.
 */
static void cal_run(struct wlan_work *work)
{
    struct ath9k *ah = ((struct ath9k_cal_work *)(void *)work)->ah;
    struct ath9k_cal *cal = &ah->cal;
    uint64_t now = tick_count(), interval;
    int longcal = 0, shortcal = 0, ret;

    if (!cal->running)
        return;
    /* A scan's channels say nothing of the home channel's. */
    if (!ah->mac.irq_on || ah->recover.resetting || ah->recover.pending ||
        (ah->rx.filters & WLAN_FILTER_OTHER_BSS)) {
        cal_queue(cal, now + ATH9K_ANI_MS);
        return;
    }
    if (elapsed(now, cal->last_long,
                (cal->flags & ATH9K_CAL_NF_INTF) ? LONG_CAL_INTF_MS : ATH9K_LONG_CAL_MS)) {
        longcal = 1;
        cal->last_long = now;
    }
    if (!cal->done) {
        if (elapsed(now, cal->last_short, ATH9K_SHORT_CAL_MS)) {
            shortcal = 1;
            cal->last_short = now;
            cal->last_full = now;
        }
    } else if (elapsed(now, cal->last_full, ATH9K_RESTART_CAL_MS)) {
        cal->done = (uint8_t)iq_restart(ah);
        if (cal->done)
            cal->last_full = now;
    }
    if (elapsed(now, cal->last_ani, ATH9K_ANI_MS)) {
        cal->last_ani = now;
        ath9k_ani_monitor(ah);
    }
    cal->temp = (uint8_t)REG_READ_FIELD(ah, AR_PHY_BB_THERM_ADC_4,
                                        AR_PHY_BB_THERM_ADC_4_LATEST_THERM_VALUE);
    if (longcal || shortcal) {
        ret = calibrate(ah, longcal);
        if (ret < 0) {
            cal->done = 0;
            klog(KLOG_WARN, "ath9k", "cal: noise floor not loaded; resetting");
            ath9k_reset_schedule(ah, ATH9K_RESET_CAL);
            return;
        }
        cal->done = (uint8_t)ret;
    }

    interval = CAL_WORK_MAX_MS;
    if (ATH9K_ANI_MS < interval)
        interval = ATH9K_ANI_MS;
    if (!cal->done && ATH9K_SHORT_CAL_MS < interval)
        interval = ATH9K_SHORT_CAL_MS;
    cal_queue(cal, now + interval);
}

/** Start the periodic work from now, its short calibrations not done, as
 * ath_start_ani does after a reset; a run already queued is cancelled. */
void ath9k_cal_start(struct ath9k *ah)
{
    struct ath9k_cal *cal = &ah->cal;
    uint64_t now = tick_count();
    unsigned i;

    for (i = 0; i < 2; i++) {
        wlan_cancel_work(&cal->work[i].work);
        cal->work[i].work.run = cal_run;
        cal->work[i].work.wlan = ah->radio.wlan;
        cal->work[i].ah = ah;
    }
    cal->last_long = cal->last_short = cal->last_ani = cal->last_full = now;
    cal->done = 0;
    cal->running = 1;
    cal_queue(cal, now + ATH9K_ANI_MS);
}

/** Stop the periodic work, waiting for a run in progress. */
void ath9k_cal_stop(struct ath9k *ah)
{
    ah->cal.running = 0;
    wlan_cancel_work(&ah->cal.work[0].work);
    wlan_cancel_work(&ah->cal.work[1].work);
}
