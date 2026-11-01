/*
 * antenna.c: WLAN switch tables, LNA diversity and combining for AR9003.
 * Follows Linux v6.16 ar9003_hw_ant_ctrl_apply, set_bt_ant_diversity and
 * antenna.c. The packet-based scan compares the two LNAs and A+B and A-B;
 * the chip row supplies the bias and the LNA switching threshold.
 */
/*
 * Copyright (c) 2010-2011 Atheros Communications Inc.
 * Copyright (c) 2012 Qualcomm Atheros, Inc.
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

#define ANT_LNA2    1
#define ANT_LNA1    2
#define ANT_SUM     3
#define ANT_SAMPLES 512
#define ANT_HOLD    16

/** The chip's antenna-control override, or the EEPROM's byte. */
static unsigned ant_control(const struct ath9k *ah)
{
    const struct ath9k_ant_params *p = ah->hw.chip->ant;

    return p && p->control ? p->control : ah->eeprom.image.base_ext1.ant_div_control;
}

/** Clear one bounded window of receive measurements. */
static void ant_clear(struct ath9k_ant *ant)
{
    ant->total = 0;
    ant->main_rssi_sum = ant->alt_rssi_sum = 0;
    ant->main_count = ant->alt_count = 0;
}

/** Write the selected LNAs and the chip's bias, preserving other fields.
 * The kernel lock is held; the register window is awake. */
static void ant_write(struct ath9k *ah)
{
    const struct ath9k_ant_params *p = ah->hw.chip->ant;
    uint32_t mask = AR_PHY_ANT_DIV_MAIN_LNACONF | AR_PHY_ANT_DIV_ALT_LNACONF |
                    AR_PHY_ANT_DIV_MAIN_GAINTB | AR_PHY_ANT_DIV_ALT_GAINTB |
                    AR_PHY_ANT_FAST_DIV_BIAS;
    uint32_t value = SM(ah->ant.main_conf, AR_PHY_ANT_DIV_MAIN_LNACONF) |
                     SM(ah->ant.alt_conf, AR_PHY_ANT_DIV_ALT_LNACONF);

    if (p)
        value |= SM(p->bias[ah->ant.main_conf][ah->ant.alt_conf], AR_PHY_ANT_FAST_DIV_BIAS);
    REG_RMW(ah, AR_PHY_MC_GAIN_CTRL, value, mask);
}

/** Choose diversity from the chip, the card wiring and the EEPROM.
 * Attach under the kernel lock; this touches no registers. */
void ath9k_ant_init(struct ath9k *ah)
{
    struct ath9k_ant *ant = &ah->ant;
    unsigned ctl = ant_control(ah);
    int capable = (ah->hw.chip->caps & ATH9K_CHIP_ANT_COMB) != 0;

    ant->combining = capable && ah->hw.chip->ant && (ctl & 0xc0) == 0xc0;
    if ((ah->bus.flags & (ATH9K_PCI_1ANT | ATH9K_PCI_2ANT)) &&
        !(ah->bus.flags & ATH9K_PCI_BT_ANT_DIV))
        ant->combining = 0;
    ant->diversity = capable && (ant->combining || (ah->bus.flags & ATH9K_PCI_BT_ANT_DIV));
    ant->main_conf = ANT_LNA1;
    ant->alt_conf = ANT_LNA2;
    ant->count = ant->scan_step = 0;
    ant->scanning = ant->combining;
    ant->rssi[0] = ant->rssi[1] = ant->rssi[2] = ant->rssi[3] = 0;
    ant_clear(ant);
}

/** Restore the EEPROM's switch tables and WLAN diversity after reset.
 * Bluetooth arbitration and interrupts stay disabled. Worker context under
 * the kernel lock, with the chip awake and receive disabled. */
void ath9k_ant_set(struct ath9k *ah)
{
    const struct ar9300_modal_eep_header *modal = &ah->eeprom.image.modalHeader2G;
    unsigned ctl = ant_control(ah);
    uint32_t value;
    static const uint32_t chain_reg[3] = {AR_PHY_SWITCH_CHAIN_0, AR_PHY_SWITCH_CHAIN_1,
                                          AR_PHY_SWITCH_CHAIN_2};
    unsigned i;

    ath9k_ant_init(ah);
    REG_RMW_FIELD(ah, AR_PHY_SWITCH_COM, AR_SWITCH_TABLE_COM_ALL, modal->antCtrlCommon);
    REG_RMW_FIELD(ah, AR_PHY_SWITCH_COM_2, AR_SWITCH_TABLE_COM2_ALL, modal->antCtrlCommon2);
    for (i = 0; i < 3; i++)
        if (ah->hw.chip->chainmask & (1u << i))
            REG_RMW_FIELD(ah, chain_reg[i], AR_SWITCH_TABLE_ALL, modal->antCtrlChain[i]);
    if (ah->hw.chip->caps & ATH9K_CHIP_MCI) {
        REG_WRITE(ah, AR_MCI_INTERRUPT_EN, 0);
        REG_WRITE(ah, AR_MCI_INTERRUPT_RX_MSG_EN, 0);
        REG_WRITE(ah, AR_BTCOEX_CTRL, 0);
        REG_RMW(ah, AR_PHY_GLB_CONTROL,
                SM(modal->switchcomspdt, AR_SWITCH_TABLE_COM_SPDT_ALL) | AR_BTCOEX_CTRL_SPDT_ENABLE,
                AR_SWITCH_TABLE_COM_SPDT_ALL | AR_BTCOEX_CTRL_BT_OWN_SPDT_CTRL);
    }
    if (!(ah->hw.chip->caps & ATH9K_CHIP_ANT_COMB))
        return;
    value = (ctl & 0x3f) << AR_ANT_DIV_CTRL_ALL_S;
    if (ah->ant.diversity)
        value |= AR_ANT_DIV_ENABLE | AR_PHY_ANT_SW_RX_PROT;
    REG_RMW(ah, AR_PHY_MC_GAIN_CTRL, value,
            AR_ANT_DIV_CTRL_ALL | AR_ANT_DIV_ENABLE | AR_PHY_ANT_SW_RX_PROT);
    REG_RMW(ah, AR_PHY_CCK_DETECT, ah->ant.diversity ? AR_FAST_DIV_ENABLE : 0, AR_FAST_DIV_ENABLE);
    REG_RMW(ah, AR_PHY_RESTART, ah->ant.diversity ? AR_PHY_RESTART_ENABLE_DIV_M2FLAG : 0,
            AR_PHY_RESTART_ENABLE_DIV_M2FLAG);
    REG_RMW(ah, AR_BTCOEX_WL_LNADIV, ah->ant.diversity ? AR_BTCOEX_WL_LNADIV_FORCE_ON : 0,
            AR_BTCOEX_WL_LNADIV_FORCE_ON);
    if (ah->ant.combining || !ah->ant.diversity)
        ant_write(ah);
    ant_clear(&ah->ant);
}

/** Compare a bounded window of valid main/alternate RSSI samples.
 * Interrupt context under the kernel lock. Stale configurations, bad frames
 * and missing RSSI are ignored. No switch occurs inside an aggregate. */
void ath9k_ant_rx(struct ath9k *ah, const struct ar9003_rxs *rxs)
{
    struct ath9k_ant *ant = &ah->ant;
    unsigned main_rssi, alt_rssi, conf, other, best;

    if (!ant->combining || ah->power.suspended || !(rxs->status11 & AR_RxDone) ||
        !(rxs->status11 & AR_RxFrameOK))
        return;
    main_rssi = MS(rxs->status1, AR_RxRSSIAnt00);
    alt_rssi = MS(rxs->status1, AR_RxRSSIAnt01);
    conf = MS(rxs->status1, AR_RxRSSIAnt02);
    if (!main_rssi || main_rssi > 127 || !alt_rssi || alt_rssi > 127 ||
        ((conf >> 2) & 3) != ant->main_conf || (conf & 3) != ant->alt_conf)
        return;
    if (ant->total < ANT_SAMPLES) {
        ant->main_rssi_sum += main_rssi;
        ant->alt_rssi_sum += alt_rssi;
        if (((conf >> 4) & 3) == ant->main_conf)
            ant->main_count++;
        else
            ant->alt_count++;
        ant->total++;
    }
    if (ant->total < ANT_SAMPLES || (rxs->status11 & AR_RxMoreAggr))
        return;
    ant->rssi[ant->main_conf] = ant->main_rssi_sum / ant->total;
    ant->rssi[ant->alt_conf] = ant->alt_rssi_sum / ant->total;
    if (ant->scanning) {
        if (ant->scan_step < 2) {
            ant->scan_step++;
            ant->alt_conf = ant->scan_step == 1 ? ANT_SUM : 0;
        } else {
            ant->main_conf =
                ant->rssi[ANT_LNA2] > ant->rssi[ANT_LNA1] + ah->hw.chip->ant->switch_delta
                    ? ANT_LNA2
                    : ANT_LNA1;
            other = ant->main_conf == ANT_LNA1 ? ANT_LNA2 : ANT_LNA1;
            best = other;
            if (ant->rssi[ANT_SUM] > ant->rssi[best])
                best = ANT_SUM;
            if (ant->rssi[0] > ant->rssi[best])
                best = 0;
            ant->alt_conf = best;
            ant->scanning = ant->count = 0;
        }
    } else if (++ant->count >= ANT_HOLD) {
        ant->count = ant->scan_step = 0;
        ant->scanning = 1;
        ant->alt_conf = ant->main_conf == ANT_LNA1 ? ANT_LNA2 : ANT_LNA1;
    }
    ant_write(ah);
    ant_clear(ant);
}
