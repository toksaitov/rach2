/*
 * chips.c: the table of the AR9003 chips the ath9k driver serves, a row
 * each, and the lookups pci.c and hw.c make in it.
 *
 * A row gives a chip's PCI device ID, the MAC version and revisions its
 * AR_SREV reads, what it has, the per-card flags of ath9k's pci.c table for
 * it, and the two steps it does its own way: choosing its initial-value
 * tables by its revision, and its gain tables by the EEPROM's gain modes,
 * each as ath9k's ar9003_hw.c does for the chip. Adding a chip is a row
 * here, its initvals and its IDs, and a test of its caps wherever ath9k
 * tests for it in code the family shares.
 */

/*
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
#include "../../../lib/kdef.h"
#include "ar9565_1p1_initvals.h"

/** Point `ini` at the `rows` rows of `columns` words at `words`. */
static void ini_set(struct ath9k_ini *ini, const uint32_t *words, unsigned rows, unsigned columns)
{
    ini->words = words;
    ini->rows = rows;
    ini->columns = columns;
}

/* ini_set for the static two-dimensional table `t`. */
#define INI_SET(ini, t) \
    ini_set(&(ini), &(t)[0][0], sizeof(t) / sizeof((t)[0]), sizeof((t)[0]) / sizeof((t)[0][0]))

/* ------------------------------------------------------------------ */
/* The AR9565 (QCA9565).                                               */
/* ------------------------------------------------------------------ */

/* Its version and revisions (ath9k's reg.h); 1.0.1 takes 1.0's tables. */
#define AR_SREV_VERSION_9565      0x2C0
#define AR_SREV_REVISION_9565_10  0
#define AR_SREV_REVISION_9565_101 1
#define AR_SREV_REVISION_9565_11  2

static const struct ath9k_chip_rev ar9565_revs[] = {{AR_SREV_REVISION_9565_10, "1.0"},
                                                    {AR_SREV_REVISION_9565_101, "1.0.1"},
                                                    {AR_SREV_REVISION_9565_11, "1.1"},
                                                    {0, NULL}};

/* The AR9565 cards of ath9k's pci.c table, in its order; the G50-70's are
 * 17aa:3026 and 17aa:4026. Any other card has the last row's flags. */
static const struct ath9k_subsys ar9565_subsys[] = {
    {0x168c, 0x3028, ATH9K_PCI_CUS252 | ATH9K_PCI_2ANT | ATH9K_PCI_BT_ANT_DIV},
    {0x1a3b, 0x2176, ATH9K_PCI_CUS252 | ATH9K_PCI_2ANT | ATH9K_PCI_BT_ANT_DIV},
    {0x105b, 0xe068, ATH9K_PCI_1ANT},
    {0x1a3b, 0x218a, ATH9K_PCI_1ANT},
    {0x1a3b, 0x2f8a, ATH9K_PCI_1ANT},
    {0x168c, 0x3025, ATH9K_PCI_1ANT | ATH9K_PCI_BT_ANT_DIV},
    {0x168c, 0x3026, ATH9K_PCI_1ANT | ATH9K_PCI_BT_ANT_DIV},
    {0x168c, 0x302b, ATH9K_PCI_1ANT | ATH9K_PCI_BT_ANT_DIV},
    {0x105b, 0xe069, ATH9K_PCI_1ANT | ATH9K_PCI_BT_ANT_DIV},
    {0x1a3b, 0x213a, ATH9K_PCI_1ANT | ATH9K_PCI_BT_ANT_DIV},
    {0x1a3b, 0x213c, ATH9K_PCI_1ANT | ATH9K_PCI_BT_ANT_DIV},
    {0x103c, 0x18e3, ATH9K_PCI_1ANT | ATH9K_PCI_BT_ANT_DIV},
    {0x103c, 0x217f, ATH9K_PCI_1ANT | ATH9K_PCI_BT_ANT_DIV},
    {0x103c, 0x2005, ATH9K_PCI_1ANT | ATH9K_PCI_BT_ANT_DIV},
    {0x1028, 0x020c, ATH9K_PCI_1ANT | ATH9K_PCI_BT_ANT_DIV},
    {0x144d, 0x411a, ATH9K_PCI_2ANT | ATH9K_PCI_BT_ANT_DIV},
    {0x144d, 0x411b, ATH9K_PCI_2ANT | ATH9K_PCI_BT_ANT_DIV},
    {0x144d, 0x411c, ATH9K_PCI_2ANT | ATH9K_PCI_BT_ANT_DIV},
    {0x144d, 0x411d, ATH9K_PCI_2ANT | ATH9K_PCI_BT_ANT_DIV},
    {0x144d, 0x411e, ATH9K_PCI_2ANT | ATH9K_PCI_BT_ANT_DIV},
    {0x144d, 0x4129, ATH9K_PCI_2ANT | ATH9K_PCI_BT_ANT_DIV},
    {0x144d, 0x412a, ATH9K_PCI_2ANT | ATH9K_PCI_BT_ANT_DIV},
    {0x168c, 0x3027, ATH9K_PCI_2ANT | ATH9K_PCI_BT_ANT_DIV},
    {0x168c, 0x302c, ATH9K_PCI_2ANT | ATH9K_PCI_BT_ANT_DIV},
    {0x1a3b, 0x2130, ATH9K_PCI_2ANT | ATH9K_PCI_BT_ANT_DIV},
    {0x1a3b, 0x213b, ATH9K_PCI_2ANT | ATH9K_PCI_BT_ANT_DIV},
    {0x1a3b, 0x2182, ATH9K_PCI_2ANT | ATH9K_PCI_BT_ANT_DIV},
    {0x1a3b, 0x218b, ATH9K_PCI_2ANT | ATH9K_PCI_BT_ANT_DIV},
    {0x1a3b, 0x218c, ATH9K_PCI_2ANT | ATH9K_PCI_BT_ANT_DIV},
    {0x1a3b, 0x2f82, ATH9K_PCI_2ANT | ATH9K_PCI_BT_ANT_DIV},
    {0x105b, 0xe07f, ATH9K_PCI_2ANT | ATH9K_PCI_BT_ANT_DIV},
    {0x105b, 0xe08f, ATH9K_PCI_2ANT | ATH9K_PCI_BT_ANT_DIV},
    {0x105b, 0xe081, ATH9K_PCI_2ANT | ATH9K_PCI_BT_ANT_DIV},
    {0x105b, 0xe091, ATH9K_PCI_2ANT | ATH9K_PCI_BT_ANT_DIV},
    {0x105b, 0xe099, ATH9K_PCI_2ANT | ATH9K_PCI_BT_ANT_DIV},
    {0x17aa, 0x3026, ATH9K_PCI_2ANT | ATH9K_PCI_BT_ANT_DIV},
    {0x17aa, 0x4026, ATH9K_PCI_2ANT | ATH9K_PCI_BT_ANT_DIV},
    {0x1043, 0x85f2, ATH9K_PCI_2ANT | ATH9K_PCI_BT_ANT_DIV},
    {0x1028, 0x020e, ATH9K_PCI_2ANT | ATH9K_PCI_BT_ANT_DIV | ATH9K_PCI_LED_ACT_HI},
    {0, 0, ATH9K_PCI_BT_ANT_DIV}};

/**
 * Fill ah->hw's tables for the AR9565's revision, as ath9k's
 * ar9003_hw_init_mode_regs does: 1.1 takes 1.1's tables, 1.0 and 1.0.1
 * take 1.0's. The transmit table is the lowest output power's until
 * ar9565_init_gain_tables chooses by the EEPROM. The PCIe table is the one
 * ath9k writes around sleep, since it sets pll_pwrsave to 7 for this chip.
 */
static void ar9565_init_mode_regs(struct ath9k *ah)
{
    struct ath9k_hw *hw = &ah->hw;
    unsigned i;

    for (i = 0; i < ATH9K_INI_STAGES; i++) {
        ini_set(&hw->mac[i], NULL, 0, 0);
        ini_set(&hw->bb[i], NULL, 0, 0);
        ini_set(&hw->radio[i], NULL, 0, 0);
        ini_set(&hw->soc[i], NULL, 0, 0);
    }
    if (hw->mac_rev >= AR_SREV_REVISION_9565_11) {
        INI_SET(hw->mac[ATH9K_INI_CORE], ar9565_1p1_mac_core);
        INI_SET(hw->mac[ATH9K_INI_POST], ar9565_1p1_mac_postamble);
        INI_SET(hw->bb[ATH9K_INI_CORE], ar9565_1p1_baseband_core);
        INI_SET(hw->bb[ATH9K_INI_POST], ar9565_1p1_baseband_postamble);
        INI_SET(hw->radio[ATH9K_INI_CORE], ar9565_1p1_radio_core);
        INI_SET(hw->radio[ATH9K_INI_POST], ar9565_1p1_radio_postamble);
        INI_SET(hw->soc[ATH9K_INI_PRE], ar9565_1p1_soc_preamble);
        INI_SET(hw->soc[ATH9K_INI_POST], ar9565_1p1_soc_postamble);
        INI_SET(hw->rx_gain, ar9565_1p1_Common_rx_gain_table);
        INI_SET(hw->tx_gain, ar9565_1p1_Modes_lowest_ob_db_tx_gain_table);
        INI_SET(hw->pcie, ar9565_1p1_pciephy_clkreq_disable_L1);
        INI_SET(hw->fast_clock, ar9565_1p1_modes_fast_clock);
        INI_SET(hw->japan_2484, ar9565_1p1_baseband_core_txfir_coeff_japan_2484);
    } else {
        INI_SET(hw->mac[ATH9K_INI_CORE], ar9565_1p0_mac_core);
        INI_SET(hw->mac[ATH9K_INI_POST], ar9565_1p0_mac_postamble);
        INI_SET(hw->bb[ATH9K_INI_CORE], ar9565_1p0_baseband_core);
        INI_SET(hw->bb[ATH9K_INI_POST], ar9565_1p0_baseband_postamble);
        INI_SET(hw->radio[ATH9K_INI_CORE], ar9565_1p0_radio_core);
        INI_SET(hw->radio[ATH9K_INI_POST], ar9565_1p0_radio_postamble);
        INI_SET(hw->soc[ATH9K_INI_PRE], ar9565_1p0_soc_preamble);
        INI_SET(hw->soc[ATH9K_INI_POST], ar9565_1p0_soc_postamble);
        INI_SET(hw->rx_gain, ar9565_1p0_Common_rx_gain_table);
        INI_SET(hw->tx_gain, ar9565_1p0_Modes_lowest_ob_db_tx_gain_table);
        INI_SET(hw->pcie, ar9565_1p0_pciephy_clkreq_disable_L1);
        INI_SET(hw->fast_clock, ar9565_1p0_modes_fast_clock);
        INI_SET(hw->japan_2484, ar9565_1p0_baseband_core_txfir_coeff_japan_2484);
    }
}

/**
 * Choose the AR9565's gain tables for the EEPROM's modes, as ath9k's
 * ar9003_tx_gain_table_mode0 to _mode7 and ar9003_rx_gain_table_mode0 to
 * _mode3 do for it: transmit modes 0 and 2 take the low output power's
 * table, 1 the high's and 3 the high power one, and the others keep the
 * lowest's; receive mode 1 takes the table without an external LNA, and
 * every other the common one.
 */
static void ar9565_init_gain_tables(struct ath9k *ah, unsigned tx, unsigned rx)
{
    struct ath9k_hw *hw = &ah->hw;
    int v11 = hw->mac_rev == AR_SREV_REVISION_9565_11;

    switch (tx) {
    case 0:
    case 2:
        if (v11)
            INI_SET(hw->tx_gain, ar9565_1p1_modes_low_ob_db_tx_gain_table);
        else
            INI_SET(hw->tx_gain, ar9565_1p0_modes_low_ob_db_tx_gain_table);
        break;
    case 1:
        if (v11)
            INI_SET(hw->tx_gain, ar9565_1p1_modes_high_ob_db_tx_gain_table);
        else
            INI_SET(hw->tx_gain, ar9565_1p0_modes_high_ob_db_tx_gain_table);
        break;
    case 3:
        if (v11)
            INI_SET(hw->tx_gain, ar9565_1p1_modes_high_power_tx_gain_table);
        else
            INI_SET(hw->tx_gain, ar9565_1p0_modes_high_power_tx_gain_table);
        break;
    default:
        break;
    }
    if (rx == 1) {
        if (v11)
            INI_SET(hw->rx_gain, ar9565_1p1_common_wo_xlna_rx_gain_table);
        else
            INI_SET(hw->rx_gain, ar9565_1p0_common_wo_xlna_rx_gain_table);
    } else if (v11)
        INI_SET(hw->rx_gain, ar9565_1p1_Common_rx_gain_table);
    else
        INI_SET(hw->rx_gain, ar9565_1p0_Common_rx_gain_table);
}

static const struct ath9k_chip_ops ar9565_ops = {ar9565_init_mode_regs, ar9565_init_gain_tables};

/* ath9k's antenna.c, ath_ant_div_conf_fast_divbias, diversity group 3. */
static const struct ath9k_ant_params ar9565_ant = {
    {{0, 1, 0x39, 1}, {2, 0, 0x3f, 2}, {3, 3, 0, 3}, {1, 6, 1, 0}},
    3,
    AR9300_EEP_ANTDIV_CONTROL_DEFAULT_VALUE};

/* One chain, 2.4 GHz alone, LNA combining, MCI and the AR9462's design
 * (ath9k's ath9k_hw_fill_cap_info), and 14 GPIO lines (AR9565_NUM_GPIO). */
static const struct ath9k_chip ar9565 = {"AR9565",
                                         0x0036,
                                         AR_SREV_VERSION_9565,
                                         ar9565_revs,
                                         ATH9K_CHIP_2GHZ | ATH9K_CHIP_PCOEM | ATH9K_CHIP_JUPITER |
                                             ATH9K_CHIP_ANT_COMB | ATH9K_CHIP_MCI,
                                         0x1,
                                         14,
                                         ar9565_subsys,
                                         &ar9565_ops,
                                         &ar9565_ant};

/* ------------------------------------------------------------------ */
/* The table and its lookups.                                          */
/* ------------------------------------------------------------------ */

const struct ath9k_chip *const ath9k_chips[] = {&ar9565, NULL};

/** The row of PCI device `device`, or NULL. */
const struct ath9k_chip *ath9k_chip_find(uint16_t device)
{
    unsigned i;

    for (i = 0; ath9k_chips[i]; i++)
        if (ath9k_chips[i]->device == device)
            return ath9k_chips[i];
    return NULL;
}

/** The revision `mac_rev` of `chip`, or NULL. */
const struct ath9k_chip_rev *ath9k_chip_rev(const struct ath9k_chip *chip, uint32_t mac_rev)
{
    const struct ath9k_chip_rev *rev;

    for (rev = chip->revs; rev->name; rev++)
        if (rev->mac_rev == mac_rev)
            return rev;
    return NULL;
}

/** The flags `chip`'s row gives a card with these subsystem IDs: those of
 * its first row that names them, or of its last. */
unsigned ath9k_chip_flags(const struct ath9k_chip *chip, uint16_t subsys_vendor,
                          uint16_t subsys_device)
{
    const struct ath9k_subsys *row;

    for (row = chip->subsys; row->vendor; row++)
        if (row->vendor == subsys_vendor && row->device == subsys_device)
            break;
    return row->flags;
}
