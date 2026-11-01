/*
 * phy.c: the ath9k driver's radio: the initial-value tables of a channel,
 * the synthesizer, the spur mitigation and the transmit power.
 *
 * The chip has no firmware, so the driver tunes it. A reset writes the
 * chip's tables for 2.4 GHz HT20, then the registers the driver sets
 * itself, then the synthesizer; a channel change of a running chip
 * retunes the synthesizer while the baseband gives up the radio bus. The
 * transmit power of each rate is the EEPROM's target, capped by the
 * caller's limit, the channel's and the conformance test limits, and the
 * chip compensates it for temperature itself from the slope and the
 * calibration temperature written here.
 *
 * The functions follow ath9k's ar9003_phy.c (ar9003_hw_set_channel,
 * ar9003_hw_process_ini and its helpers, the spur mitigation, the delta
 * slope, the radio bus), hw.c (ath9k_hw_channel_change,
 * ath9k_hw_get_delta_slope_vals, ath9k_hw_apply_txpower) and
 * ar9003_eeprom.c (ath9k_hw_ar9300_set_txpower, its power table and
 * register writes, ar9003_hw_calibration_apply and
 * ar9003_hw_power_control_override), each named at its function, under
 * ath9k's notice:
 *
 * Copyright (c) 2010-2011 Atheros Communications Inc.
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

/* The 2.4 GHz band's channels the synthesizer is set for: 1 to 14. */
#define FREQ_2G_FIRST 2412
#define FREQ_2G_LAST  2484

/* Powers in half dB (hw.h): the most one rate may have, the most the
 * caller and the channel may ask, and what each further chain takes off
 * (eeprom.h's POWER_CORRECTION_FOR_TWO_CHAIN and _THREE_CHAIN). */
#define MAX_RATE_POWER         63
#define MAX_COMBINED_POWER     254
#define POWER_CORR_TWO_CHAIN   6
#define POWER_CORR_THREE_CHAIN 10

/* The microseconds every baseband activation waits on top of the chip's
 * own delay (hw.h's BASE_ACTIVATE_DELAY), and the delta slope's scale
 * (COEF_SCALE_S). */
#define BASE_ACTIVATE_DELAY 100
#define COEF_SCALE_S        24

/* The spur mitigation's reach on a 20 MHz channel, in MHz. */
#define SPUR_RANGE 10

/** The frequency in MHz of an EEPROM frequency bin of the 2.4 GHz band,
 * as ath9k_hw_fbin2freq has it; an unused bin, 0xff, stays 0xff. */
static int fbin2freq(uint8_t fbin)
{
    if (fbin == 0xff)
        return 0xff;
    return 2300 + fbin;
}

/** The absolute value of `v`. */
static int iabs(int v)
{
    return v < 0 ? -v : v;
}

/** The chains of the chip's row (ath9k's rxchainmask and txchainmask,
 * which are the chip's own). */
static unsigned chainmask(const struct ath9k *ah)
{
    return ah->hw.chip->chainmask;
}

/** Wait the chip's synthesizer delay, `hw_delay` in units of 100 ns, as
 * ath9k_hw_synth_delay does on a full-rate channel. */
static void synth_delay(struct ath9k *ah, uint32_t hw_delay)
{
    ATH9K_UDELAY(ah, hw_delay / 10 + BASE_ACTIVATE_DELAY);
}

/* ------------------------------------------------------------------ */
/* The tables and the channel's registers (ar9003_phy.c).              */
/* ------------------------------------------------------------------ */

/** Write column `column` of `ini`, or its column 1 when it has one value
 * for every mode, as ar9003_hw_prog_ini does; an absent table writes
 * nothing. */
static void prog_ini(struct ath9k *ah, const struct ath9k_ini *ini, unsigned column)
{
    if (!ini->words)
        return;
    if (column >= ini->columns)
        column = 1;
    ath9k_write_ini(ah, ini, column);
}

/** Override the tables with what the driver sets itself, as
 * ar9003_hw_override_ini does: receive held off until the MAC enables it,
 * multicast key search by address alone, and on a chip of the AR9462's
 * design the regulator shared with Bluetooth. It also learns which
 * transmit calibrations the tables enable, for calib.c. */
static void override_ini(struct ath9k *ah)
{
    uint32_t val;
    uint8_t enabled = ATH9K_CAL_ENABLE_TXIQ | ATH9K_CAL_ENABLE_TXIQ_ON_AGC;

    REG_SET_BIT(ah, AR_DIAG_SW, AR_DIAG_RX_DIS | AR_DIAG_RX_ABORT);

    val = REG_READ(ah, AR_PCU_MISC_MODE2) & ~AR_ADHOC_MCAST_KEYID_ENABLE;
    val |= AR_AGG_WEP_ENABLE_FIX | AR_AGG_WEP_ENABLE | AR_PCU_MISC_MODE2_CFP_IGNORE;
    REG_WRITE(ah, AR_PCU_MISC_MODE2, val);

    if (ah->hw.chip->caps & ATH9K_CHIP_JUPITER) {
        REG_WRITE(ah, AR_GLB_SWREG_DISCONT_MODE, AR_GLB_SWREG_DISCONT_EN_BT_WLAN);
        if (!REG_READ_FIELD(ah, AR_PHY_TX_IQCAL_CONTROL_0,
                            AR_PHY_TX_IQCAL_CONTROL_0_ENABLE_TXIQ_CAL))
            enabled &= ~ATH9K_CAL_ENABLE_TXIQ;
    }
    if (REG_READ(ah, AR_PHY_CL_CAL_CTL) & AR_PHY_CL_CAL_ENABLE)
        enabled |= ATH9K_CAL_ENABLE_TXCL;
    ah->cal.enabled = enabled;
}

/** Set the baseband for 20 MHz HT, keeping what the tables set, and the
 * MAC's timeouts, as ar9003_hw_set_channel_regs and
 * ath9k_hw_set11nmac2040 do for an HT20 channel. */
static void set_channel_regs(struct ath9k *ah)
{
    uint32_t phymode;

    phymode = AR_PHY_GC_HT_EN | AR_PHY_GC_SHORT_GI_40 | AR_PHY_GC_SINGLE_HT_LTF1 |
              (REG_READ(ah, AR_PHY_GEN_CTRL) & AR_PHY_GC_ENABLE_DAC_FIFO);
    phymode |= REG_READ(ah, AR_PHY_GEN_CTRL);
    phymode &= ~AR_PHY_GC_GF_DETECT_EN;
    REG_WRITE(ah, AR_PHY_GEN_CTRL, phymode);

    REG_WRITE(ah, AR_2040_MODE, 0);
    REG_WRITE(ah, AR_GTXTO, 25 << AR_GTXTO_TIMEOUT_LIMIT_S);
    REG_WRITE(ah, AR_CST, 0xF << AR_CST_TIMEOUT_LIMIT_S);
}

/** Set the receive, calibration and self-generated frames' chains, as
 * ar9003_hw_set_chain_masks does. */
static void set_chain_masks(struct ath9k *ah, unsigned rx, unsigned tx)
{
    if (chainmask(ah) == 5)
        REG_SET_BIT(ah, AR_PHY_ANALOG_SWAP, AR_PHY_SWAP_ALT_CHAIN);
    REG_WRITE(ah, AR_PHY_RX_CHAINMASK, rx);
    REG_WRITE(ah, AR_PHY_CAL_CHAINMASK, rx);
    REG_WRITE(ah, AR_SELFGEN_MASK, tx);
}

/**
 * Write the tables for `chan`, as ar9003_hw_process_ini does on a 2.4 GHz
 * HT20 channel: each stage's SOC, MAC, baseband and radio tables in turn,
 * the receive gain table, the transmit gain table, channel 14's CCK
 * filter, then the driver's overrides, the channel's registers, the
 * chains and the transmit power. The AR9300's doubler fix and the 5 GHz
 * fast clock belong to chips and a band no row has.
 */
void ath9k_phy_process_ini(struct ath9k *ah, const struct wlan_channel *chan)
{
    struct ath9k_hw *hw = &ah->hw;
    unsigned i;

    for (i = 0; i < ATH9K_INI_STAGES; i++) {
        prog_ini(ah, &hw->soc[i], ATH9K_INI_2G_HT20);
        prog_ini(ah, &hw->mac[i], ATH9K_INI_2G_HT20);
        prog_ini(ah, &hw->bb[i], ATH9K_INI_2G_HT20);
        prog_ini(ah, &hw->radio[i], ATH9K_INI_2G_HT20);
    }
    ath9k_write_ini(ah, &hw->rx_gain, 1);
    ath9k_write_ini(ah, &hw->tx_gain, ATH9K_INI_2G_HT20);
    if (chan->frequency == 2484)
        prog_ini(ah, &hw->japan_2484, 1);

    override_ini(ah);
    set_channel_regs(ah);
    set_chain_masks(ah, chainmask(ah), chainmask(ah));
    ath9k_phy_set_txpower(ah, chan, ah->phy.power_limit);
}

/** Set the RF mode for 2.4 GHz, CCK and OFDM both, as
 * ar9003_hw_set_rfmode does. */
void ath9k_phy_set_rfmode(struct ath9k *ah, const struct wlan_channel *chan)
{
    (void)chan;
    REG_WRITE(ah, AR_PHY_MODE, AR_PHY_MODE_DYNAMIC);
}

/** Mark the baseband inactive, as ar9003_hw_mark_phy_inactive does. */
void ath9k_phy_mark_inactive(struct ath9k *ah)
{
    REG_WRITE(ah, AR_PHY_ACTIVE, AR_PHY_ACTIVE_DIS);
}

/** Split the scaled coefficient `coef` into the mantissa and exponent the
 * chip takes, as ath9k_hw_get_delta_slope_vals does. */
static void delta_slope_vals(uint32_t coef, uint32_t *man, uint32_t *exp)
{
    uint32_t e, m;

    for (e = 31; e > 0; e--)
        if ((coef >> e) & 1)
            break;
    e = 14 - (e - COEF_SCALE_S);
    m = coef + (1u << (COEF_SCALE_S - e - 1));
    *man = m >> (COEF_SCALE_S - e);
    *exp = e - 16;
}

/** Set the delta slope coefficients of `chan`, for the normal and the
 * short guard interval, as ar9003_hw_set_delta_slope does. */
void ath9k_phy_set_delta_slope(struct ath9k *ah, const struct wlan_channel *chan)
{
    uint32_t coef, man, exp;

    coef = 0x64000000u / chan->frequency;
    delta_slope_vals(coef, &man, &exp);
    REG_RMW_FIELD(ah, AR_PHY_TIMING3, AR_PHY_TIMING3_DSC_MAN, man);
    REG_RMW_FIELD(ah, AR_PHY_TIMING3, AR_PHY_TIMING3_DSC_EXP, exp);

    coef = (9 * coef) / 10;
    delta_slope_vals(coef, &man, &exp);
    REG_RMW_FIELD(ah, AR_PHY_SGI_DELTA, AR_PHY_SGI_DSC_MAN, man);
    REG_RMW_FIELD(ah, AR_PHY_SGI_DELTA, AR_PHY_SGI_DSC_EXP, exp);
}

/** Activate the baseband and wait for the synthesizer to settle, as
 * ar9003_hw_init_bb does. */
void ath9k_phy_init_bb(struct ath9k *ah, const struct wlan_channel *chan)
{
    uint32_t delay = REG_READ(ah, AR_PHY_RX_DELAY) & AR_PHY_RX_DELAY_DELAY;

    (void)chan;
    REG_WRITE(ah, AR_PHY_ACTIVE, AR_PHY_ACTIVE_EN);
    synth_delay(ah, delay);
}

/** Ask the baseband for the radio bus, as ar9003_hw_rfbus_req does.
 * -ETIMEDOUT when it is not granted within ATH9K_WAIT_TIMEOUT. */
int ath9k_phy_rfbus_req(struct ath9k *ah)
{
    REG_WRITE(ah, AR_PHY_RFBUS_REQ, AR_PHY_RFBUS_REQ_EN);
    return ath9k_wait(ah, AR_PHY_RFBUS_GRANT, AR_PHY_RFBUS_GRANT_EN, AR_PHY_RFBUS_GRANT_EN,
                      ATH9K_WAIT_TIMEOUT);
}

/** Wait for the synthesizer and give the radio bus back, as
 * ar9003_hw_rfbus_done does. */
void ath9k_phy_rfbus_done(struct ath9k *ah)
{
    uint32_t delay = REG_READ(ah, AR_PHY_RX_DELAY) & AR_PHY_RX_DELAY_DELAY;

    synth_delay(ah, delay);
    REG_WRITE(ah, AR_PHY_RFBUS_REQ, 0);
}

/* ------------------------------------------------------------------ */
/* The synthesizer (ar9003_phy.c).                                     */
/* ------------------------------------------------------------------ */

/**
 * Program the synthesizer for `chan`, as ar9003_hw_set_channel does for a
 * 2.4 GHz channel in fractional mode with the 40 MHz reference: the
 * channel is (3/4) * 40 MHz * (chansel + chanfrac / 2^17), so chansel is
 * 4f / 120 and chanfrac the remainder in 2^17ths. The synthesizer is
 * written once with the load bit clear and once with it set. A chip that
 * divides otherwise, as ath9k's AR_SREV list shows (the AR9300, AR9462
 * and AR9580 take CHANSEL_2G), needs a cap for it.
 */
int ath9k_phy_set_channel(struct ath9k *ah, const struct wlan_channel *chan)
{
    uint32_t freq = chan->frequency, sel, frac, reg32;

    if (!(ah->hw.chip->caps & ATH9K_CHIP_2GHZ) || freq < FREQ_2G_FIRST || freq > FREQ_2G_LAST)
        return -EINVAL;

    sel = (freq * 4) / 120;
    frac = (((freq * 4) % 120) * 0x20000) / 120;
    sel = (sel << 17) | frac;

    REG_WRITE(ah, AR_PHY_SYNTH_CONTROL, 1u << 29);
    REG_RMW_FIELD(ah, AR_PHY_65NM_CH0_SYNTH4, AR_PHY_SYNTH4_LONG_SHIFT_SELECT, 1);
    reg32 = (sel << 2) | (1u << 30);
    REG_WRITE(ah, AR_PHY_65NM_CH0_SYNTH7, reg32);
    REG_WRITE(ah, AR_PHY_65NM_CH0_SYNTH7, reg32 | (1u << 31));

    ah->phy.channel = *chan;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Spur mitigation (ar9003_phy.c).                                     */
/* ------------------------------------------------------------------ */

/** Clear every OFDM spur field, as ar9003_hw_spur_ofdm_clear does. */
static void spur_ofdm_clear(struct ath9k *ah)
{
    REG_RMW_FIELD(ah, AR_PHY_TIMING4, AR_PHY_TIMING4_ENABLE_SPUR_FILTER, 0);
    REG_RMW_FIELD(ah, AR_PHY_TIMING11, AR_PHY_TIMING11_SPUR_FREQ_SD, 0);
    REG_RMW_FIELD(ah, AR_PHY_TIMING11, AR_PHY_TIMING11_SPUR_DELTA_PHASE, 0);
    REG_RMW_FIELD(ah, AR_PHY_SFCORR_EXT, AR_PHY_SFCORR_EXT_SPUR_SUBCHANNEL_SD, 0);
    REG_RMW_FIELD(ah, AR_PHY_TIMING11, AR_PHY_TIMING11_USE_SPUR_FILTER_IN_AGC, 0);
    REG_RMW_FIELD(ah, AR_PHY_TIMING11, AR_PHY_TIMING11_USE_SPUR_FILTER_IN_SELFCOR, 0);
    REG_RMW_FIELD(ah, AR_PHY_TIMING4, AR_PHY_TIMING4_ENABLE_SPUR_RSSI, 0);
    REG_RMW_FIELD(ah, AR_PHY_SPUR_REG, AR_PHY_SPUR_REG_EN_VIT_SPUR_RSSI, 0);
    REG_RMW_FIELD(ah, AR_PHY_SPUR_REG, AR_PHY_SPUR_REG_ENABLE_NF_RSSI_SPUR_MIT, 0);

    REG_RMW_FIELD(ah, AR_PHY_SPUR_REG, AR_PHY_SPUR_REG_ENABLE_MASK_PPM, 0);
    REG_RMW_FIELD(ah, AR_PHY_TIMING4, AR_PHY_TIMING4_ENABLE_PILOT_MASK, 0);
    REG_RMW_FIELD(ah, AR_PHY_TIMING4, AR_PHY_TIMING4_ENABLE_CHAN_MASK, 0);
    REG_RMW_FIELD(ah, AR_PHY_PILOT_SPUR_MASK, AR_PHY_PILOT_SPUR_MASK_CF_PILOT_MASK_IDX_A, 0);
    REG_RMW_FIELD(ah, AR_PHY_SPUR_MASK_A, AR_PHY_SPUR_MASK_A_CF_PUNC_MASK_IDX_A, 0);
    REG_RMW_FIELD(ah, AR_PHY_CHAN_SPUR_MASK, AR_PHY_CHAN_SPUR_MASK_CF_CHAN_MASK_IDX_A, 0);
    REG_RMW_FIELD(ah, AR_PHY_PILOT_SPUR_MASK, AR_PHY_PILOT_SPUR_MASK_CF_PILOT_MASK_A, 0);
    REG_RMW_FIELD(ah, AR_PHY_CHAN_SPUR_MASK, AR_PHY_CHAN_SPUR_MASK_CF_CHAN_MASK_A, 0);
    REG_RMW_FIELD(ah, AR_PHY_SPUR_MASK_A, AR_PHY_SPUR_MASK_A_CF_PUNC_MASK_A, 0);
    REG_RMW_FIELD(ah, AR_PHY_SPUR_REG, AR_PHY_SPUR_REG_MASK_RATE_CNTL, 0);
}

/** The subcarrier index of a spur `offset` MHz from the channel, as the
 * mask fields take it (ar9003_hw_spur_ofdm's mask_index). */
static uint32_t spur_mask_index(int offset)
{
    int index = (offset * 16) / 5;

    if (index < 0)
        index--;
    return (uint32_t)index & 0x7f;
}

/** Turn on the OFDM spur filter and mask A for a spur `offset` MHz from
 * the channel, as ar9003_hw_spur_ofdm_work and ar9003_hw_spur_ofdm do on
 * an HT20 channel. A chip with ATH9K_CHIP_SPUR_AB leaves the filter out of
 * self-correlation for a spur within 10 MHz of channel 6, 2437 MHz. The
 * offsets are multiplied, not shifted as in ath9k, since a negative value
 * shifted left is undefined in C. */
static void spur_ofdm(struct ath9k *ah, int offset, unsigned synth)
{
    uint32_t freq_sd = (uint32_t)((offset * 512) / 11) & 0x3ff;
    uint32_t delta_phase = (uint32_t)((offset * 262144) / 5) & 0xfffff;
    uint32_t mask_index = spur_mask_index(offset);

    REG_RMW_FIELD(ah, AR_PHY_TIMING4, AR_PHY_TIMING4_ENABLE_SPUR_FILTER, 0x1);
    REG_RMW_FIELD(ah, AR_PHY_TIMING11, AR_PHY_TIMING11_SPUR_FREQ_SD, freq_sd);
    REG_RMW_FIELD(ah, AR_PHY_TIMING11, AR_PHY_TIMING11_SPUR_DELTA_PHASE, delta_phase);
    REG_RMW_FIELD(ah, AR_PHY_SFCORR_EXT, AR_PHY_SFCORR_EXT_SPUR_SUBCHANNEL_SD, 0);
    REG_RMW_FIELD(ah, AR_PHY_TIMING11, AR_PHY_TIMING11_USE_SPUR_FILTER_IN_AGC, 0x1);
    if (!((ah->hw.chip->caps & ATH9K_CHIP_SPUR_AB) && synth == 2437))
        REG_RMW_FIELD(ah, AR_PHY_TIMING11, AR_PHY_TIMING11_USE_SPUR_FILTER_IN_SELFCOR, 0x1);
    REG_RMW_FIELD(ah, AR_PHY_TIMING4, AR_PHY_TIMING4_ENABLE_SPUR_RSSI, 0x1);
    REG_RMW_FIELD(ah, AR_PHY_SPUR_REG, AR_PHY_SPUR_REG_SPUR_RSSI_THRESH, 34);
    REG_RMW_FIELD(ah, AR_PHY_SPUR_REG, AR_PHY_SPUR_REG_EN_VIT_SPUR_RSSI, 1);
    if (REG_READ_FIELD(ah, AR_PHY_MODE, AR_PHY_MODE_DYNAMIC) == 0x1)
        REG_RMW_FIELD(ah, AR_PHY_SPUR_REG, AR_PHY_SPUR_REG_ENABLE_NF_RSSI_SPUR_MIT, 1);

    REG_RMW_FIELD(ah, AR_PHY_SPUR_REG, AR_PHY_SPUR_REG_ENABLE_MASK_PPM, 0x1);
    REG_RMW_FIELD(ah, AR_PHY_TIMING4, AR_PHY_TIMING4_ENABLE_PILOT_MASK, 0x1);
    REG_RMW_FIELD(ah, AR_PHY_TIMING4, AR_PHY_TIMING4_ENABLE_CHAN_MASK, 0x1);
    REG_RMW_FIELD(ah, AR_PHY_PILOT_SPUR_MASK, AR_PHY_PILOT_SPUR_MASK_CF_PILOT_MASK_IDX_A,
                  mask_index);
    REG_RMW_FIELD(ah, AR_PHY_SPUR_MASK_A, AR_PHY_SPUR_MASK_A_CF_PUNC_MASK_IDX_A, mask_index);
    REG_RMW_FIELD(ah, AR_PHY_CHAN_SPUR_MASK, AR_PHY_CHAN_SPUR_MASK_CF_CHAN_MASK_IDX_A, mask_index);
    REG_RMW_FIELD(ah, AR_PHY_PILOT_SPUR_MASK, AR_PHY_PILOT_SPUR_MASK_CF_PILOT_MASK_A, 0xc);
    REG_RMW_FIELD(ah, AR_PHY_CHAN_SPUR_MASK, AR_PHY_CHAN_SPUR_MASK_CF_CHAN_MASK_A, 0xc);
    REG_RMW_FIELD(ah, AR_PHY_SPUR_MASK_A, AR_PHY_SPUR_MASK_A_CF_PUNC_MASK_A, 0xa0);
    REG_RMW_FIELD(ah, AR_PHY_SPUR_REG, AR_PHY_SPUR_REG_MASK_RATE_CNTL, 0xff);
}

/** Set mask B for a second spur `offset` MHz from the channel, as
 * ar9003_hw_spur_ofdm_9565 does; the punctured mask B takes mask A's
 * fields. */
static void spur_ofdm_b(struct ath9k *ah, int offset)
{
    uint32_t mask_index = spur_mask_index(offset);

    REG_RMW_FIELD(ah, AR_PHY_PILOT_SPUR_MASK, AR_PHY_PILOT_SPUR_MASK_CF_PILOT_MASK_IDX_B,
                  mask_index);
    REG_RMW_FIELD(ah, AR_PHY_SPUR_MASK_B, AR_PHY_SPUR_MASK_A_CF_PUNC_MASK_IDX_A, mask_index);
    REG_RMW_FIELD(ah, AR_PHY_CHAN_SPUR_MASK, AR_PHY_CHAN_SPUR_MASK_CF_CHAN_MASK_IDX_B, mask_index);
    REG_RMW_FIELD(ah, AR_PHY_PILOT_SPUR_MASK, AR_PHY_PILOT_SPUR_MASK_CF_PILOT_MASK_B, 0xe);
    REG_RMW_FIELD(ah, AR_PHY_CHAN_SPUR_MASK, AR_PHY_CHAN_SPUR_MASK_CF_CHAN_MASK_B, 0xe);
    REG_RMW_FIELD(ah, AR_PHY_SPUR_MASK_B, AR_PHY_SPUR_MASK_A_CF_PUNC_MASK_A, 0xa0);
}

/**
 * Mitigate the spurs the EEPROM lists for 2.4 GHz, as
 * ar9003_hw_spur_mitigate_ofdm does on an HT20 channel: the first spur
 * within 10 MHz of the channel gets the filter and mask A, and on a chip
 * with ATH9K_CHIP_SPUR_AB the spur listed after it, if also near, mask B.
 * Such a chip has no CCK spur filter; one without it needs ath9k's
 * ar9003_hw_spur_mitigate_mrc_cck, which no row needs yet.
 */
void ath9k_phy_spur_mitigate(struct ath9k *ah, const struct wlan_channel *chan)
{
    const uint8_t *spur = ah->eeprom.image.modalHeader2G.spurChans;
    int synth = chan->frequency, offset;
    unsigned i;

    if (spur[0] == 0)
        return;
    spur_ofdm_clear(ah);
    for (i = 0; i < AR_EEPROM_MODAL_SPURS && spur[i]; i++) {
        offset = fbin2freq(spur[i]) - synth;
        if (iabs(offset) >= SPUR_RANGE)
            continue;
        spur_ofdm(ah, offset, (unsigned)synth);
        if ((ah->hw.chip->caps & ATH9K_CHIP_SPUR_AB) && i < AR_EEPROM_MODAL_SPURS - 1) {
            offset = fbin2freq(spur[i + 1]) - synth;
            if (iabs(offset) < SPUR_RANGE)
                spur_ofdm_b(ah, offset);
        }
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Transmit power (ar9003_eeprom.c).                                   */
/* ------------------------------------------------------------------ */

/** Lower each of `pwr`'s entries `first` to `last` to at most `cap`. */
static void cap_rates(uint8_t *pwr, unsigned first, unsigned last, unsigned cap)
{
    unsigned i;

    for (i = first; i <= last; i++)
        if (pwr[i] > cap)
            pwr[i] = (uint8_t)cap;
}

/* Four powers of half dB as one register of AR_PHY_POWER_TX_RATE, the
 * first in the low byte, each in 6 bits (ar9003_hw_tx_power_regwrite's
 * POW_SM). */
#define POW4(a, b, c, d)                                                                         \
    (((uint32_t)((a) & 0x3f)) | ((uint32_t)((b) & 0x3f) << 8) | ((uint32_t)((c) & 0x3f) << 16) | \
     ((uint32_t)((d) & 0x3f) << 24))

/** Write each rate's power, as ar9003_hw_tx_power_regwrite does. */
static void tx_power_regwrite(struct ath9k *ah, const uint8_t *p)
{
    REG_WRITE(ah, AR_PHY_TX_FORCED_GAIN, 0);
    /* OFDM 6, 9, 12, 18; 24, 36, 48, 54. */
    REG_WRITE(ah, AR_PHY_POWER_TX_RATE(0),
              POW4(p[ALL_TARGET_LEGACY_6_24], p[ALL_TARGET_LEGACY_6_24], p[ALL_TARGET_LEGACY_6_24],
                   p[ALL_TARGET_LEGACY_6_24]));
    REG_WRITE(ah, AR_PHY_POWER_TX_RATE(1),
              POW4(p[ALL_TARGET_LEGACY_6_24], p[ALL_TARGET_LEGACY_36], p[ALL_TARGET_LEGACY_48],
                   p[ALL_TARGET_LEGACY_54]));
    /* CCK 1L, reserved, 2L, 2S; 5.5L, 5.5S, 11L, 11S. */
    REG_WRITE(ah, AR_PHY_POWER_TX_RATE(2),
              POW4(p[ALL_TARGET_LEGACY_1L_5L], 0, p[ALL_TARGET_LEGACY_1L_5L],
                   p[ALL_TARGET_LEGACY_1L_5L]));
    REG_WRITE(ah, AR_PHY_POWER_TX_RATE(3),
              POW4(p[ALL_TARGET_LEGACY_1L_5L], p[ALL_TARGET_LEGACY_5S], p[ALL_TARGET_LEGACY_11L],
                   p[ALL_TARGET_LEGACY_11S]));
    /* Duplicated frames of HT40: dup40 CCK, dup40 OFDM, ext20 CCK, ext20
     * OFDM. */
    REG_WRITE(ah, AR_PHY_POWER_TX_RATE(8),
              POW4(p[ALL_TARGET_LEGACY_1L_5L], p[ALL_TARGET_LEGACY_6_24],
                   p[ALL_TARGET_LEGACY_1L_5L], p[ALL_TARGET_LEGACY_6_24]));
    /* HT20 MCS 0/8/16, 1-3/9-11/17-19, 4, 5; 6, 7, 12, 13; 14, 15, 20, 21. */
    REG_WRITE(ah, AR_PHY_POWER_TX_RATE(4),
              POW4(p[ALL_TARGET_HT20_0_8_16], p[ALL_TARGET_HT20_1_3_9_11_17_19],
                   p[ALL_TARGET_HT20_4], p[ALL_TARGET_HT20_5]));
    REG_WRITE(ah, AR_PHY_POWER_TX_RATE(5),
              POW4(p[ALL_TARGET_HT20_6], p[ALL_TARGET_HT20_7], p[ALL_TARGET_HT20_12],
                   p[ALL_TARGET_HT20_13]));
    REG_WRITE(ah, AR_PHY_POWER_TX_RATE(9),
              POW4(p[ALL_TARGET_HT20_14], p[ALL_TARGET_HT20_15], p[ALL_TARGET_HT20_20],
                   p[ALL_TARGET_HT20_21]));
    /* HT20 22, 23, HT40 22, 23. */
    REG_WRITE(ah, AR_PHY_POWER_TX_RATE(10),
              POW4(p[ALL_TARGET_HT20_22], p[ALL_TARGET_HT20_23], p[ALL_TARGET_HT40_22],
                   p[ALL_TARGET_HT40_23]));
    /* HT40 as HT20. */
    REG_WRITE(ah, AR_PHY_POWER_TX_RATE(6),
              POW4(p[ALL_TARGET_HT40_0_8_16], p[ALL_TARGET_HT40_1_3_9_11_17_19],
                   p[ALL_TARGET_HT40_4], p[ALL_TARGET_HT40_5]));
    REG_WRITE(ah, AR_PHY_POWER_TX_RATE(7),
              POW4(p[ALL_TARGET_HT40_6], p[ALL_TARGET_HT40_7], p[ALL_TARGET_HT40_12],
                   p[ALL_TARGET_HT40_13]));
    REG_WRITE(ah, AR_PHY_POWER_TX_RATE(11),
              POW4(p[ALL_TARGET_HT40_14], p[ALL_TARGET_HT40_15], p[ALL_TARGET_HT40_20],
                   p[ALL_TARGET_HT40_21]));
}

/** `ya` and `yb` at `xa` and `xb` interpolated at `x`, rounded as
 * ar9003_eeprom.c's interpolate does. */
static int interpolate(int x, int xa, int xb, int ya, int yb)
{
    int bf = 2 * (yb - ya) * (x - xa) / (xb - xa);

    return ya + bf / 2 + bf % 2;
}

/* What a calibration pier of the EEPROM holds for one chain. */
struct pier {
    int freq;
    int correction; /* the power measured against the target, half dB */
    int temperature;
    int nf_cal;
};

/**
 * Apply the calibration piers' power correction and temperature at
 * `freq`, as ar9003_hw_calibration_apply and
 * ar9003_hw_power_control_override do for 2.4 GHz: for each chain the
 * nearest piers below and above, interpolated when both are within 1000
 * MHz; the correction into each chain's open-loop gain delta, open-loop
 * control on, the EEPROM's temperature slope and the pier's temperature,
 * from which the chip compensates the power for temperature itself. The
 * EEPROM's noise floor at the piers goes to phy.nf_cal for calib.c. The
 * correction is masked to its field, which ath9k's REG_RMW leaves to the
 * value, so that a negative one cannot set the bits above it.
 */
static void calibration_apply(struct ath9k *ah, int freq)
{
    const struct ar9300_eeprom *eep = &ah->eeprom.image;
    static const uint32_t tpc11[AR9300_MAX_CHAINS] = {AR_PHY_TPC_11_B0, AR_PHY_TPC_11_B1,
                                                      AR_PHY_TPC_11_B2};
    static const uint32_t tpc6[AR9300_MAX_CHAINS] = {AR_PHY_TPC_6_B0, AR_PHY_TPC_6_B1,
                                                     AR_PHY_TPC_6_B2};
    struct pier lo, hi, p, out[AR9300_MAX_CHAINS];
    int chain, i;

    for (chain = 0; chain < AR9300_MAX_CHAINS; chain++) {
        lo.freq = 0;
        hi.freq = 100000;
        lo.correction = lo.temperature = lo.nf_cal = 0;
        hi.correction = hi.temperature = hi.nf_cal = 0;
        for (i = 0; i < AR9300_NUM_2G_CAL_PIERS; i++) {
            const struct ar9300_cal_data_per_freq_op_loop *d = &eep->calPierData2G[chain][i];
            int fdiff;

            p.freq = fbin2freq(eep->calFreqPier2G[i]);
            p.correction = d->refPower;
            p.temperature = d->tempMeas;
            p.nf_cal = d->rxTempMeas ? N2DBM(d->rxNoisefloorCal) : 0;
            fdiff = freq - p.freq;
            if (fdiff <= 0 && (hi.freq <= 0 || hi.freq >= 100000 || fdiff > freq - hi.freq))
                hi = p;
            if (fdiff >= 0 && (lo.freq <= 0 || fdiff < freq - lo.freq))
                lo = p;
        }
        if (hi.freq == lo.freq)
            out[chain] = lo;
        else if (freq - lo.freq < 1000) {
            if (hi.freq - freq < 1000) {
                out[chain].correction =
                    interpolate(freq, lo.freq, hi.freq, lo.correction, hi.correction);
                out[chain].temperature =
                    interpolate(freq, lo.freq, hi.freq, lo.temperature, hi.temperature);
                out[chain].nf_cal = interpolate(freq, lo.freq, hi.freq, lo.nf_cal, hi.nf_cal);
            } else
                out[chain] = lo;
        } else if (hi.freq - freq < 1000)
            out[chain] = hi;
        else
            out[chain].correction = out[chain].temperature = out[chain].nf_cal = 0;
    }

    for (chain = 0; chain < AR9300_MAX_CHAINS; chain++)
        if (chain == 0 || (chainmask(ah) & (1u << chain)))
            REG_RMW(ah, tpc11[chain],
                    ((uint32_t)out[chain].correction << AR_PHY_TPC_OLPC_GAIN_DELTA_S) &
                        AR_PHY_TPC_OLPC_GAIN_DELTA,
                    AR_PHY_TPC_OLPC_GAIN_DELTA);
    for (chain = 0; chain < AR9300_MAX_CHAINS; chain++)
        if (chain == 0 || (chainmask(ah) & (1u << chain)))
            REG_RMW(ah, tpc6[chain], 3u << AR_PHY_TPC_6_ERROR_EST_MODE_S,
                    AR_PHY_TPC_6_ERROR_EST_MODE);
    REG_RMW_FIELD(ah, AR_PHY_TPC_19, AR_PHY_TPC_19_ALPHA_THERM, eep->modalHeader2G.tempSlope);
    REG_RMW_FIELD(ah, AR_PHY_TPC_18, AR_PHY_TPC_18_THERM_CAL_VALUE, out[0].temperature);

    for (chain = 0; chain < AR9300_MAX_CHAINS; chain++)
        ah->phy.nf_cal[chain] = (int16_t)out[chain].nf_cal;
}

/**
 * Set each rate's transmit power for `chan`, as ath9k_hw_apply_txpower
 * and ath9k_hw_ar9300_set_txpower do with transmit power control off and
 * without PAPRD: the EEPROM's targets; the cap of the smaller of `limit`
 * and the channel's maximum, less the antenna's gain and a correction for
 * each further chain; each CCK, OFDM and HT20 rate at most its
 * conformance test limit; the registers; the calibration piers'
 * correction; and ACKs and CTSs at the CCK target. A negative antenna gain
 * reduces nothing, where ath9k would read it as unsigned.
 */
void ath9k_phy_set_txpower(struct ath9k *ah, const struct wlan_channel *chan, int limit)
{
    uint8_t pwr[ar9300RateSize];
    int cap = MAX_COMBINED_POWER, reduction, gain;
    unsigned freq = chan->frequency, scaled, mask = chainmask(ah), chains;
    unsigned ctl;

    ah->phy.power_limit = (int16_t)(limit > 0 ? limit : 0);
    if (chan->max_power > 0 && chan->max_power * 2 < cap)
        cap = chan->max_power * 2;
    if (limit > 0 && limit < cap)
        cap = limit;

    gain = ah->eeprom.image.modalHeader2G.antennaGain;
    reduction = gain > 0 ? gain : 0;
    chains = (mask & 1) + ((mask >> 1) & 1) + ((mask >> 2) & 1);
    if (chains == 2)
        reduction += POWER_CORR_TWO_CHAIN;
    else if (chains == 3)
        reduction += POWER_CORR_THREE_CHAIN;
    scaled = cap > reduction ? (unsigned)(cap - reduction) : 0;
    if (scaled > MAX_RATE_POWER)
        scaled = MAX_RATE_POWER;

    ath9k_eeprom_target_power(ah, freq, pwr);
    ctl = ath9k_eeprom_ctl_limit(ah, freq, CTL_11B);
    cap_rates(pwr, ALL_TARGET_LEGACY_1L_5L, ALL_TARGET_LEGACY_11S, ctl < scaled ? ctl : scaled);
    ctl = ath9k_eeprom_ctl_limit(ah, freq, CTL_11G);
    cap_rates(pwr, ALL_TARGET_LEGACY_6_24, ALL_TARGET_LEGACY_54, ctl < scaled ? ctl : scaled);
    ctl = ath9k_eeprom_ctl_limit(ah, freq, CTL_2GHT20);
    cap_rates(pwr, ALL_TARGET_HT20_0_8_16, ALL_TARGET_HT20_23, ctl < scaled ? ctl : scaled);

    tx_power_regwrite(ah, pwr);
    calibration_apply(ah, (int)freq);
    REG_WRITE(ah, AR_TPC,
              SM(pwr[ALL_TARGET_LEGACY_1L_5L], AR_TPC_ACK) |
                  SM(pwr[ALL_TARGET_LEGACY_1L_5L], AR_TPC_CTS) | SM(0x3f, AR_TPC_CHIRP) |
                  SM(0x3f, AR_TPC_RPT));
    REG_WRITE(ah, AR_PHY_PWRTX_MAX, 0);
    k_memcpy(ah->phy.target, pwr, sizeof(pwr));
}

/* ------------------------------------------------------------------ */
/* A channel change without a reset (hw.c).                            */
/* ------------------------------------------------------------------ */

/** Whether queue `q` holds a frame, as ath9k_hw_numtxpending sees it. */
static int tx_pending(struct ath9k *ah, unsigned q)
{
    if (REG_READ(ah, AR_QSTS(q)) & AR_Q_STS_PEND_FR_CNT)
        return 1;
    return (REG_READ(ah, AR_Q_TXE) & (1u << q)) != 0;
}

/**
 * Retune a running chip to `chan`, as ath9k_hw_do_fastcc and
 * ath9k_hw_channel_change do when the band and mode stay the same, which
 * on a chip of 2.4 GHz HT20 alone they always do: no table is written
 * again, so the RF mode, the board's values and the reset-time
 * calibrations stay. The MAC's clock rate stays too. A change that fails
 * leaves the channel and its calibration data as they were.
 */
int ath9k_phy_channel_change(struct ath9k *ah, const struct wlan_channel *chan)
{
    unsigned q;
    int r;

    for (q = 0; q < AR_NUM_QCU; q++)
        if (tx_pending(ah, q))
            return -EBUSY;
    r = ath9k_phy_rfbus_req(ah);
    if (r)
        return r;
    set_channel_regs(ah);
    r = ath9k_phy_set_channel(ah, chan);
    if (r) {
        ath9k_phy_rfbus_done(ah);
        return r;
    }
    ath9k_cal_set_channel(ah, chan);
    ath9k_phy_set_txpower(ah, chan, ah->phy.power_limit);
    ath9k_phy_set_delta_slope(ah, chan);
    ath9k_phy_spur_mitigate(ah, chan);
    ath9k_phy_init_bb(ah, chan);
    ath9k_phy_rfbus_done(ah);

    (void)ath9k_nf_load(ah);
    ath9k_nf_start(ah);
    return 0;
}
