/*
 * hw.c: the chip's power, its resets and its identity in the ath9k driver:
 * attach, which brings a card up and reads what it is, and the reset that
 * puts it on a channel.
 *
 * The sequences follow ath9k's hw.c (Linux v6.16): __ath9k_hw_init,
 * ath9k_hw_read_revisions, ath9k_hw_chip_test, ath9k_hw_set_reset_reg and
 * the resets under it, ath9k_hw_setpower, ath9k_hw_init_pll,
 * ath9k_hw_disable and ath9k_hw_reset with the hw.c steps it takes. Where
 * ath9k tests for a chip by name, this file tests the row's ATH9K_CHIP_*
 * bits, and takes the path of the chips the table has.
 */

#include "ath9k.h"
#include "../../../klog.h"
#include "../../../lib/kerrno.h"

/* hw.h's times in microseconds: the longest the chip takes to wake, and
 * the PLL's settling. */
#define POWER_UP_TIME        10000
#define RTC_PLL_SETTLE_DELAY 100

/* hw.h's receive signal threshold at reset. */
#define INIT_RSSI_THR 0x00000700

/* The MAC's clock in MHz at 2.4 GHz and, with the AR9003's fast clock, at
 * 5 GHz (hw.h's ATH9K_CLOCK_RATE_2GHZ_OFDM and ATH9K_CLOCK_FAST_RATE_5GHZ_OFDM,
 * both 44). */
#define CLOCK_RATE 44

/* The baseband's chip ID register (reg.h's AR_PHY_CHIP_ID), which attach
 * reads once as ath9k does. */
#define AR_PHY_CHIP_ID 0x9818

/* reg_mci.h's MCI receive message enable, which sleep clears. */
#define AR_MCI_INTERRUPT_RX_MSG_EN 0x183c

/* The legacy rates every radio of the family offers, 100 kbit/s. */
static const uint16_t legacy_rates[] = {10, 20, 55, 110, 60, 90, 120, 180, 240, 360, 480, 540};

/** Whether the row has every bit of `caps`. */
static int has(const struct ath9k *ah, unsigned caps)
{
    return (ah->hw.chip->caps & caps) == caps;
}

/* ------------------------------------------------------------------ */
/* Resets.                                                             */
/* ------------------------------------------------------------------ */

/**
 * Reset the MAC warm or cold through AR_RTC_RC, as ath9k_hw_set_reset does
 * for the AR9003 family: wake the chip, clear the host interface if a
 * PCIe completion timed out, pulse the reset and wait for it to end.
 * Returns 0 or -EIO.
 */
static int set_reset(struct ath9k *ah, enum ath9k_reset_type type)
{
    uint32_t flags;

    REG_WRITE(ah, AR_WA, ah->hw.wa);
    ATH9K_UDELAY(ah, 10);
    REG_WRITE(ah, AR_RTC_FORCE_WAKE, AR_RTC_FORCE_WAKE_EN | AR_RTC_FORCE_WAKE_ON_INT);
    if (REG_READ(ah, AR_INTR_SYNC_CAUSE) &
        (AR_INTR_SYNC_LOCAL_TIMEOUT | AR_INTR_SYNC_RADM_CPL_TIMEOUT)) {
        REG_WRITE(ah, AR_INTR_SYNC_ENABLE, 0);
        REG_WRITE(ah, AR_RC, AR_RC_HOSTIF);
    }
    flags = AR_RTC_RC_MAC_WARM;
    if (type == ATH9K_RESET_COLD)
        flags |= AR_RTC_RC_MAC_COLD;
    REG_WRITE(ah, AR_RTC_RC, flags);
    ATH9K_UDELAY(ah, 50);
    REG_WRITE(ah, AR_RTC_RC, 0);
    if (ath9k_wait(ah, AR_RTC_RC, AR_RTC_RC_M, 0, ATH9K_WAIT_TIMEOUT) != 0) {
        klog(KLOG_ERR, "ath9k", "reset: failed: AR_RTC_RC (0x%x) stays in reset", AR_RTC_RC);
        return -EIO;
    }
    REG_WRITE(ah, AR_RC, 0);
    return 0;
}

/**
 * Reset the RTC itself and with it the whole chip, as
 * ath9k_hw_set_reset_power_on does: hold AR_RTC_RESET low for two
 * microseconds, wait for the RTC to come on, then reset the MAC warm.
 */
static int set_reset_power_on(struct ath9k *ah)
{
    REG_WRITE(ah, AR_WA, ah->hw.wa);
    ATH9K_UDELAY(ah, 10);
    REG_WRITE(ah, AR_RTC_FORCE_WAKE, AR_RTC_FORCE_WAKE_EN | AR_RTC_FORCE_WAKE_ON_INT);
    REG_WRITE(ah, AR_RTC_RESET, 0);
    ATH9K_UDELAY(ah, 2);
    REG_WRITE(ah, AR_RTC_RESET, 1);
    if (ath9k_wait(ah, AR_RTC_STATUS, AR_RTC_STATUS_M, AR_RTC_STATUS_ON, ATH9K_WAIT_TIMEOUT) != 0) {
        klog(KLOG_ERR, "ath9k", "reset: failed: AR_RTC_STATUS (0x%x) reads 0x%x, not on",
             AR_RTC_STATUS, REG_READ(ah, AR_RTC_STATUS));
        return -EIO;
    }
    return set_reset(ah, ATH9K_RESET_WARM);
}

/**
 * Reset the chip by `type`, as ath9k_hw_set_reset_reg does. The first reset
 * after attach is a power-on reset whatever `type` asks. Worker or attach.
 * Returns 0 or -EIO, which it logs.
 */
int ath9k_hw_set_reset(struct ath9k *ah, enum ath9k_reset_type type)
{
    REG_WRITE(ah, AR_WA, ah->hw.wa);
    ATH9K_UDELAY(ah, 10);
    REG_WRITE(ah, AR_RTC_FORCE_WAKE, AR_RTC_FORCE_WAKE_EN | AR_RTC_FORCE_WAKE_ON_INT);
    if (!ah->hw.reset_power_on)
        type = ATH9K_RESET_POWER_ON;
    switch (type) {
    case ATH9K_RESET_POWER_ON:
        if (set_reset_power_on(ah) != 0)
            return -EIO;
        ah->hw.reset_power_on = 1;
        return 0;
    case ATH9K_RESET_WARM:
    case ATH9K_RESET_COLD:
        return set_reset(ah, type);
    default:
        return -EIO;
    }
}

/* ------------------------------------------------------------------ */
/* Power.                                                              */
/* ------------------------------------------------------------------ */

/**
 * Wake the chip, as ath9k_hw_set_power_awake does: reset it first if the
 * RTC is shut down, then hold force-wake until the RTC reads on, setting
 * it again every 50 microseconds for at most POWER_UP_TIME.
 */
static int set_power_awake(struct ath9k *ah)
{
    unsigned i;

    REG_WRITE(ah, AR_WA, ah->hw.wa);
    ATH9K_UDELAY(ah, 10);
    if ((REG_READ(ah, AR_RTC_STATUS) & AR_RTC_STATUS_M) == AR_RTC_STATUS_SHUTDOWN &&
        ath9k_hw_set_reset(ah, ATH9K_RESET_POWER_ON) != 0)
        return -EIO;
    REG_SET_BIT(ah, AR_RTC_FORCE_WAKE, AR_RTC_FORCE_WAKE_EN);
    ATH9K_UDELAY(ah, 50);
    for (i = POWER_UP_TIME / 50; i > 0; i--) {
        if ((REG_READ(ah, AR_RTC_STATUS) & AR_RTC_STATUS_M) == AR_RTC_STATUS_ON)
            break;
        ATH9K_UDELAY(ah, 50);
        REG_SET_BIT(ah, AR_RTC_FORCE_WAKE, AR_RTC_FORCE_WAKE_EN);
    }
    if (i == 0) {
        klog(KLOG_ERR, "ath9k", "power: failed: AR_RTC_STATUS (0x%x) reads 0x%x after %u us",
             AR_RTC_STATUS, REG_READ(ah, AR_RTC_STATUS), (unsigned)POWER_UP_TIME);
        return -EIO;
    }
    REG_CLR_BIT(ah, AR_STA_ID1, AR_STA_ID1_PWR_SAV);
    return 0;
}

/** Put the chip in full sleep, its RTC shut down (ath9k_set_power_sleep). */
static void set_power_sleep(struct ath9k *ah)
{
    REG_SET_BIT(ah, AR_STA_ID1, AR_STA_ID1_PWR_SAV);
    if (has(ah, ATH9K_CHIP_JUPITER)) {
        REG_CLR_BIT(ah, AR_TIMER_MODE, 0xff);
        REG_CLR_BIT(ah, AR_NDP2_TIMER_MODE, 0xff);
        REG_CLR_BIT(ah, AR_SLP32_INC, 0xfffff);
        REG_WRITE(ah, AR_MCI_INTERRUPT_RX_MSG_EN, 0);
        ATH9K_UDELAY(ah, 100);
    }
    REG_CLR_BIT(ah, AR_RTC_FORCE_WAKE, AR_RTC_FORCE_WAKE_EN);
    REG_CLR_BIT(ah, AR_RTC_RESET, AR_RTC_RESET_EN);
    ATH9K_UDELAY(ah, 2);
    REG_WRITE(ah, AR_WA, ah->hw.wa & ~AR_WA_D3_L1_DISABLE);
}

/** Let the chip sleep between beacons, waking on an interrupt
 * (ath9k_set_power_network_sleep, with the AR9003's automatic sleep). */
static void set_power_network_sleep(struct ath9k *ah)
{
    REG_SET_BIT(ah, AR_STA_ID1, AR_STA_ID1_PWR_SAV);
    REG_CLR_BIT(ah, AR_RTC_FORCE_WAKE, AR_RTC_FORCE_WAKE_EN);
    REG_WRITE(ah, AR_WA, ah->hw.wa & ~AR_WA_D3_L1_DISABLE);
}

/**
 * Bring the chip to `mode`, as ath9k_hw_setpower does with coexistence
 * off. Nothing is written when it is in `mode` already. Returns 0, or -EIO
 * when it does not wake, which it logs.
 */
int ath9k_hw_set_power(struct ath9k *ah, enum ath9k_power_mode mode)
{
    if (ah->hw.power == mode)
        return 0;
    switch (mode) {
    case ATH9K_PM_AWAKE:
        if (set_power_awake(ah) != 0)
            return -EIO;
        break;
    case ATH9K_PM_FULL_SLEEP:
        set_power_sleep(ah);
        ah->hw.full_sleep = 1;
        break;
    case ATH9K_PM_NETWORK_SLEEP:
        set_power_network_sleep(ah);
        break;
    default:
        return -EIO;
    }
    ah->hw.power = mode;
    return 0;
}

/**
 * Program the PLL for full-rate channels, as ath9k_hw_init_pll does with
 * ar9003_hw_compute_pll_control: on a chip with ATH9K_CHIP_BB_DPLL the
 * baseband PLL first, then the RTC's, then the sleep clock.
 */
static void init_pll(struct ath9k *ah)
{
    uint32_t pll = SM(0x5, AR_RTC_9300_PLL_REFDIV) | SM(0x2c, AR_RTC_9300_PLL_DIV);

    if (has(ah, ATH9K_CHIP_BB_DPLL)) {
        /* ki 0x4 and kd 0x40, refdiv 5, nint 0x58, and phase shift 6. */
        REG_RMW_FIELD(ah, AR_CH0_BB_DPLL2, AR_CH0_BB_DPLL2_PLL_PWD, 0x1);
        REG_RMW_FIELD(ah, AR_CH0_BB_DPLL2, AR_CH0_DPLL2_KD, 0x40);
        REG_RMW_FIELD(ah, AR_CH0_BB_DPLL2, AR_CH0_DPLL2_KI, 0x4);
        REG_RMW_FIELD(ah, AR_CH0_BB_DPLL1, AR_CH0_BB_DPLL1_REFDIV, 0x5);
        REG_RMW_FIELD(ah, AR_CH0_BB_DPLL1, AR_CH0_BB_DPLL1_NINI, 0x58);
        REG_RMW_FIELD(ah, AR_CH0_BB_DPLL1, AR_CH0_BB_DPLL1_NFRAC, 0x0);
        REG_RMW_FIELD(ah, AR_CH0_BB_DPLL2, AR_CH0_BB_DPLL2_OUTDIV, 0x1);
        REG_RMW_FIELD(ah, AR_CH0_BB_DPLL2, AR_CH0_BB_DPLL2_LOCAL_PLL, 0x1);
        REG_RMW_FIELD(ah, AR_CH0_BB_DPLL2, AR_CH0_BB_DPLL2_EN_NEGTRIG, 0x1);
        REG_RMW_FIELD(ah, AR_CH0_BB_DPLL3, AR_CH0_BB_DPLL3_PHASE_SHIFT, 0x6);
        REG_RMW_FIELD(ah, AR_CH0_BB_DPLL2, AR_CH0_BB_DPLL2_PLL_PWD, 0x0);
        ATH9K_UDELAY(ah, 1000);
    }
    if (has(ah, ATH9K_CHIP_PLL_BIT18))
        pll |= 0x40000;
    REG_WRITE(ah, AR_RTC_PLL_CONTROL, pll);
    ATH9K_UDELAY(ah, RTC_PLL_SETTLE_DELAY);
    REG_WRITE(ah, AR_RTC_SLEEP_CLK, AR_RTC_FORCE_DERIVED_CLK);
}

/**
 * Set the PCIe link's power save, as ar9003_hw_configpcipowersave does,
 * where ath9k calls it: while ASPM is on, the core allowed into L1 and
 * AR_WA written again unless `power_off`, then the PCIe PHY's table, which
 * on the AR9565 keeps CLKREQ off in L1, the same for both. The chip is
 * awake.
 */
void ath9k_hw_pcie_powersave(struct ath9k *ah, int power_off)
{
    const struct ath9k_ini *t = &ah->hw.pcie;
    unsigned i;

    if (!ah->power.aspm || (ah->options & ATH9K_OPT_NO_ASPM))
        return;
    if (!power_off) {
        REG_SET_BIT(ah, AR_PCIE_PM_CTRL, AR_PCIE_PM_CTRL_ENA);
        REG_WRITE(ah, AR_WA, ah->hw.wa);
    }
    for (i = 0; i < t->rows; i++)
        REG_WRITE(ah, t->words[i * t->columns], t->words[i * t->columns + 1]);
}

/**
 * Stop the chip and leave it in full sleep, as ath9k_hw_disable and the
 * end of ath9k_stop do: wake it, reset it cold, set the PLL, set the PCIe
 * link's power save for the stop, and let it sleep. A chip that does not
 * wake is left as it is.
 */
void ath9k_hw_disable(struct ath9k *ah)
{
    if (ath9k_hw_set_power(ah, ATH9K_PM_AWAKE) != 0)
        return;
    if (ath9k_hw_set_reset(ah, ATH9K_RESET_COLD) == 0)
        init_pll(ah);
    ath9k_hw_pcie_powersave(ah, 1);
    ath9k_hw_set_power(ah, ATH9K_PM_FULL_SLEEP);
}

/* ------------------------------------------------------------------ */
/* Attach.                                                             */
/* ------------------------------------------------------------------ */

/**
 * Read the MAC version and revision from AR_SREV and check them against
 * the row, as ath9k_hw_read_revisions does for the AR9003 form. A card that
 * reads all ones is off the bus or powered down, and is refused with the
 * register named. Returns 0 or -ENODEV.
 */
static int read_revisions(struct ath9k *ah)
{
    struct ath9k_hw *hw = &ah->hw;
    uint32_t srev = REG_READ(ah, AR_SREV);

    if (srev == 0xffffffffu) {
        klog(KLOG_ERR, "ath9k", "attach: refused: AR_SREV (0x%x) reads 0xffffffff", AR_SREV);
        return -ENODEV;
    }
    if ((srev & AR_SREV_ID) != 0xff) {
        klog(KLOG_ERR, "ath9k", "attach: refused: AR_SREV (0x%x) reads 0x%x, not an AR9003",
             AR_SREV, srev);
        return -ENODEV;
    }
    hw->mac_version = (srev & AR_SREV_VERSION2) >> AR_SREV_TYPE2_S;
    hw->mac_rev = MS(srev, AR_SREV_REVISION2);
    hw->rev =
        hw->mac_version == hw->chip->mac_version ? ath9k_chip_rev(hw->chip, hw->mac_rev) : NULL;
    if (!hw->rev) {
        klog(KLOG_ERR, "ath9k",
             "attach: refused: AR_SREV (0x%x) reads version 0x%x rev %u, not an %s", AR_SREV,
             hw->mac_version, hw->mac_rev, hw->chip->name);
        return -ENODEV;
    }
    return 0;
}

/**
 * Write and read back AR_STA_ID0 with 256 counting patterns and four bit
 * patterns, then restore it, as ath9k_hw_chip_test does for the AR9003.
 * Returns 0, or -ENODEV with the first mismatch logged.
 */
static int chip_test(struct ath9k *ah)
{
    static const uint32_t pattern[4] = {0x55555555, 0xaaaaaaaa, 0x66666666, 0x99999999};
    uint32_t hold = REG_READ(ah, AR_STA_ID0), wr, rd;
    unsigned j;

    for (j = 0; j < 0x100 + 4; j++) {
        wr = j < 0x100 ? (uint32_t)j << 16 | j : pattern[j - 0x100];
        REG_WRITE(ah, AR_STA_ID0, wr);
        rd = REG_READ(ah, AR_STA_ID0);
        if (rd != wr) {
            klog(KLOG_ERR, "ath9k", "attach: failed: AR_STA_ID0 (0x%x) reads 0x%x after 0x%x",
                 AR_STA_ID0, rd, wr);
            return -ENODEV;
        }
    }
    REG_WRITE(ah, AR_STA_ID0, hold);
    ATH9K_UDELAY(ah, 100);
    return 0;
}

/**
 * The most power `freq` allows, in dBm rounded up: the highest of the
 * EEPROM's target powers there over every rate, as the test pass of
 * ath9k_hw_set_txpowerlimit finds it. That pass names no conformance test
 * (NO_CTL), so no CTL limit applies; phy.c applies them when it sets the
 * power for a channel.
 */
static int8_t channel_power(const struct ath9k *ah, unsigned freq)
{
    uint8_t t[ar9300RateSize];
    unsigned i, best = 0;

    ath9k_eeprom_target_power(ah, freq, t);
    for (i = 0; i < ar9300RateSize; i++)
        if (t[i] > best)
            best = t[i];
    return (int8_t)((best + 1) / 2);
}

/**
 * Fill the radio's capabilities, as ath9k_hw_fill_cap_info and ath9k's
 * init.c give them for HT20 at 2.4 GHz: channels 1 to 14 at the power the
 * EEPROM allows, which the registry narrows by country; the twelve legacy
 * rates; MCS 0 to 7 with the short guard interval, receive STBC of one
 * stream and no SM power save; 64 KiB A-MPDUs with 8 microseconds between
 * MPDUs; and TKIP and CCMP in hardware.
 */
static void fill_caps(struct ath9k *ah)
{
    struct wlan_band *band = &ah->radio.band;
    unsigned i;

    for (i = 0; i < 14; i++) {
        struct wlan_channel *c = &band->channels[i];

        c->number = (uint8_t)(i + 1);
        c->frequency = (uint16_t)(i == 13 ? 2484 : 2412 + 5 * i);
        c->flags = 0;
        c->max_power = channel_power(ah, c->frequency);
    }
    band->n_channels = 14;
    for (i = 0; i < sizeof(legacy_rates) / sizeof(legacy_rates[0]); i++)
        band->rates[i] = legacy_rates[i];
    band->n_rates = i;
    band->ht_supported = 1;
    band->ht_cap = 0x0020 | 0x000c | 0x0100; /* SGI 20, SM PS off, RX STBC 1 */
    band->ampdu_factor = 3;                  /* 64 KiB */
    band->ampdu_density = 6;                 /* 8 microseconds */
    for (i = 0; i < sizeof(band->mcs); i++)
        band->mcs[i] = 0;
    band->mcs[0] = 0xff;  /* MCS 0 to 7, one stream */
    band->mcs[12] = 0x01; /* the transmit set is the receive set */
    ah->radio.hw_ciphers = WLAN_CIPHER_HW_TKIP | WLAN_CIPHER_HW_CCMP;
    ah->radio.name = "ath9k";
}

/**
 * Bring the card up and read what it is, in __ath9k_hw_init's order: the
 * revision, a power-on reset, the row's tables, wake, the register test,
 * the EEPROM, the gain tables it chooses, and the radio's capabilities.
 * hw.chip is the row pci.c found. Process context. Returns 0, -ENODEV for
 * a card that is not the row's chip or does not answer, or -EIO, each
 * logged.
 */
int ath9k_hw_attach(struct ath9k *ah)
{
    struct ath9k_hw *hw = &ah->hw;
    unsigned gain;
    int rc;

    if (!hw->chip)
        return -ENODEV;
    hw->power = ATH9K_PM_FULL_SLEEP;
    hw->reset_power_on = 0;
    hw->full_sleep = 0;
    rc = read_revisions(ah);
    if (rc != 0)
        return rc;
    hw->wa = REG_READ(ah, AR_WA) | AR_WA_D3_L1_DISABLE | AR_WA_ASPM_TIMER_BASED_DISABLE;
    if (ath9k_hw_set_reset(ah, ATH9K_RESET_POWER_ON) != 0)
        return -EIO;
    if (has(ah, ATH9K_CHIP_WA_BIT22)) {
        hw->wa |= AR_WA_BIT22;
        REG_WRITE(ah, AR_WA, hw->wa);
    }
    hw->chip->ops->init_mode_regs(ah);
    if (ath9k_hw_set_power(ah, ATH9K_PM_AWAKE) != 0)
        return -EIO;
    (void)REG_READ(ah, AR_PHY_CHIP_ID);
    rc = chip_test(ah);
    if (rc != 0)
        return rc;
    if (ath9k_eeprom_init(ah) != 0)
        return -EIO;
    gain = ah->eeprom.image.baseEepHeader.txrxgain;
    hw->chip->ops->init_gain_tables(ah, (gain >> 4) & 0xf, gain & 0xf);
    fill_caps(ah);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Reset onto a channel.                                               */
/* ------------------------------------------------------------------ */

/** Reset the chip for a channel, cold when it slept or its DMA ran, and set
 * its PLL (ath9k_hw_chip_reset). */
static int chip_reset(struct ath9k *ah)
{
    enum ath9k_reset_type type = ATH9K_RESET_WARM;

    if (ah->hw.full_sleep || REG_READ(ah, AR_Q_TXE) || (REG_READ(ah, AR_CR) & AR_CR_RXE))
        type = ATH9K_RESET_COLD;
    if (ath9k_hw_set_reset(ah, type) != 0 || ath9k_hw_set_power(ah, ATH9K_PM_AWAKE) != 0)
        return -EIO;
    ah->hw.full_sleep = 0;
    init_pll(ah);
    return 0;
}

/** Bring the station's identity back after a reset: the MAC address, the
 * BSSID, the default antenna and station mode (ath9k_hw_reset_opmode). */
static void reset_opmode(struct ath9k *ah, uint32_t sta_id1, uint32_t def_antenna)
{
    REG_RMW(ah, AR_STA_ID1,
            sta_id1 | AR_STA_ID1_RTS_USE_DEF | AR_STA_ID1_CRPT_MIC_ENABLE | AR_STA_ID1_MCAST_KSRCH,
            ~(uint32_t)AR_STA_ID1_SADH_MASK);
    ath9k_mac_set_address(ah);
    REG_WRITE(ah, AR_DEF_ANTENNA, def_antenna);
    ath9k_mac_set_bssid(ah, ah->mac.bssid, ah->mac.aid);
    REG_WRITE(ah, AR_ISR, ~0u);
    REG_WRITE(ah, AR_RSSI_THR, INIT_RSSI_THR);
    REG_CLR_BIT(ah, AR_CFG, AR_CFG_AP_ADHOC_INDICATION);
    REG_RMW(ah, AR_STA_ID1, AR_STA_ID1_KSRCH_MODE, AR_STA_ID1_STA_AP | AR_STA_ID1_ADHOC);
}

/** Set the QoS fields the MAC reads from frames (ath9k_hw_init_qos). */
static void init_qos(struct ath9k *ah)
{
    REG_WRITE(ah, AR_MIC_QOS_CONTROL, 0x100aa);
    REG_WRITE(ah, AR_MIC_QOS_SELECT, 0x3210);
    REG_WRITE(ah, AR_QOS_NO_ACK,
              SM(2, AR_QOS_NO_ACK_TWO_BIT) | SM(5, AR_QOS_NO_ACK_BIT_OFF) |
                  SM(0, AR_QOS_NO_ACK_BYTE_OFF));
    REG_WRITE(ah, AR_TXOP_X, AR_TXOP_X_VAL);
    REG_WRITE(ah, AR_TXOP_0_3, 0xffffffffu);
    REG_WRITE(ah, AR_TXOP_4_7, 0xffffffffu);
    REG_WRITE(ah, AR_TXOP_8_11, 0xffffffffu);
    REG_WRITE(ah, AR_TXOP_12_15, 0xffffffffu);
}

/**
 * Set the PCU's interframe spaces and timeouts for a full-rate 2.4 GHz
 * channel, as ath9k_hw_init_global_settings does: SIFS 10; the short
 * slot of 9 microseconds, since radio.h gives the driver no slot time
 * (ath9k takes 20 from mac80211 in a BSS without short slots); the ACK and
 * CTS timeouts at the 64 and 48 microseconds ath9k pads them to whatever
 * the slot; and EIFS and the latencies as the initial values left them.
 */
static void init_global_settings(struct ath9k *ah)
{
    unsigned clk = ah->hw.clockrate, sifs = 10, slot = 9;
    unsigned ack = 64, cts = 48;
    uint32_t eifs = REG_READ(ah, AR_D_GBL_IFS_EIFS) / clk, usec = REG_READ(ah, AR_USEC);

    REG_WRITE(ah, AR_D_GBL_IFS_SIFS, (sifs - 2) * clk);
    REG_WRITE(ah, AR_D_GBL_IFS_SLOT, slot * clk);
    REG_RMW_FIELD(ah, AR_TIME_OUT, AR_TIME_OUT_ACK, ack * clk);
    REG_RMW_FIELD(ah, AR_TIME_OUT, AR_TIME_OUT_CTS, cts * clk);
    REG_WRITE(ah, AR_D_GBL_IFS_EIFS, eifs * clk);
    REG_RMW(ah, AR_USEC,
            (clk - 1) | SM(MS(usec, AR_USEC_RX_LAT), AR_USEC_RX_LAT) |
                SM(MS(usec, AR_USEC_TX_LAT), AR_USEC_TX_LAT),
            AR_USEC_TX_LAT | AR_USEC_RX_LAT | AR_USEC_USEC);
}

/**
 * Reset the chip onto `chan` in ath9k_hw_reset's order, with coexistence
 * off and no fast channel change: read the last noise floor, keep the TSF,
 * the default antenna, the 11b base rate bit and the LED's mode across a
 * full reset; write the tables, the radio's settings, the board's and the
 * antenna's settings and the station's identity; tune; set up the queues,
 * interrupts and DMA (mac.c), noise immunity, QoS and timings; turn the
 * baseband on, calibrate, load the noise floor and start a measurement.
 * Interrupts and receive stay off. Worker. Returns 0 or the failing step's
 * errno.
 */
int ath9k_hw_reset(struct ath9k *ah, const struct wlan_channel *chan)
{
    uint32_t def_antenna, sta_id1, led;
    uint64_t tsf;
    int rc;

    if (ath9k_hw_set_power(ah, ATH9K_PM_AWAKE) != 0)
        return -EIO;
    if (!ah->hw.full_sleep)
        ath9k_nf_read(ah);
    ath9k_cal_set_channel(ah, chan);
    def_antenna = REG_READ(ah, AR_DEF_ANTENNA);
    if (def_antenna == 0)
        def_antenna = 1;
    sta_id1 = REG_READ(ah, AR_STA_ID1) & AR_STA_ID1_BASE_RATE_11B;
    tsf = ath9k_mac_get_tsf(ah);
    led = REG_READ(ah, AR_CFG_LED) & (AR_CFG_LED_ASSOC_CTL | AR_CFG_LED_MODE_SEL |
                                      AR_CFG_LED_BLINK_THRESH_SEL | AR_CFG_LED_BLINK_SLOW);
    ath9k_phy_mark_inactive(ah);
    if (chip_reset(ah) != 0)
        return -EIO;
    REG_WRITE(ah, AR_TSF_L32, (uint32_t)tsf);
    REG_WRITE(ah, AR_TSF_U32, (uint32_t)(tsf >> 32));
    REG_SET_BIT(ah, AR_GPIO_INPUT_EN_VAL, AR_GPIO_JTAG_DISABLE);
    ath9k_phy_process_ini(ah, chan);
    ath9k_phy_set_rfmode(ah, chan);
    REG_RMW_FIELD(ah, AR_AES_MUTE_MASK1, AR_AES_MUTE_MASK1_FC_MGMT, 0xc7ff);
    ath9k_phy_set_delta_slope(ah, chan);
    ath9k_phy_spur_mitigate(ah, chan);
    ath9k_eeprom_set_board_values(ah, chan);
    ath9k_ant_set(ah);
    reset_opmode(ah, sta_id1, def_antenna);
    rc = ath9k_phy_set_channel(ah, chan);
    if (rc != 0)
        return rc;
    ah->hw.clockrate = CLOCK_RATE;
    ath9k_mac_init_queues(ah);
    ath9k_ani_reset(ah);
    init_qos(ah);
    init_global_settings(ah);
    REG_SET_BIT(ah, AR_STA_ID1, AR_STA_ID1_PRESERVE_SEQNUM);
    REG_WRITE(ah, AR_OBS, 8);
    REG_RMW_FIELD(ah, AR_RIMT, AR_RIMT_LAST, 500);
    REG_RMW_FIELD(ah, AR_RIMT, AR_RIMT_FIRST, 2000);
    ath9k_phy_init_bb(ah, chan);
    rc = ath9k_cal_init(ah, chan);
    if (rc != 0)
        return rc;
    ath9k_nf_load(ah);
    ath9k_nf_start(ah);
    REG_WRITE(ah, AR_CFG_LED, led | AR_CFG_SCLK_32KHZ);
    return 0;
}
