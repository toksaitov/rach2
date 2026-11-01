/*
 * ath9k_phy_hosttest.c: the ath9k driver's radio, calibrations and noise
 * immunity on the AR9003 chip model, checked by their register writes.
 *
 * host-deps: tests/host/ath9k_model.c
 * host-deps: rach2/sys/network/driver/ath9k/io.c
 * host-deps: rach2/sys/network/driver/ath9k/chips.c
 * host-deps: rach2/sys/network/driver/ath9k/phy.c
 * host-deps: rach2/sys/network/driver/ath9k/calib.c
 * host-deps: rach2/sys/network/driver/ath9k/ani.c
 * host-deps: rach2/sys/network/wireless/work.c
 * host-deps: rach2/sys/cmdline.c rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * host-deps: tests/host/compat/spinlock_host.c
 */
#include "ath9k_model.h"
#include "network/wireless/work.h"
#include "cmdline.h"
#include "lib/kerrno.h"
#include "lib/kirq.h"
#include "lib/kstring.h"
#include "klog.h"
#include "timer.h"
#include <stdio.h>

static unsigned fails;
#define CHECK(c)                                                                \
    do {                                                                        \
        if (!(c)) {                                                             \
            fprintf(stderr, "ath9k_phy_hosttest: line %d: %s\n", __LINE__, #c); \
            fails++;                                                            \
        }                                                                       \
    } while (0)

static struct ath9k ah;
static struct ath9k_model model;

/* The machine and the parts of other tasks the radio calls. */
static uint64_t now = 1000;
static uint32_t irq_flag = 1;
static unsigned warnings, resets;
static enum ath9k_reset_reason reset_why;
static uint8_t targets[ar9300RateSize];
static unsigned ctl_11b, ctl_11g, ctl_ht20;

/* The chip's behaviour the test sets. */
static int stuck_cal;   /* AR_PHY_AGC_CONTROL_CAL stays set */
static int stuck_nf;    /* AR_PHY_AGC_CONTROL_NF stays set once written */
static int keep_do_cal; /* the RX IQ sample never finishes */
static int no_grant;    /* the baseband keeps the radio bus */

uint64_t tick_count(void)
{
    return now;
}

uint32_t kirq_save(void)
{
    uint32_t flags = irq_flag;

    irq_flag = 0;
    return flags;
}

void kirq_restore(uint32_t flags)
{
    irq_flag = flags;
}

/** Count the warnings; the text is the driver's. */
void klog(int level, const char *subsystem, const char *format, ...)
{
    (void)subsystem;
    (void)format;
    if (level == KLOG_WARN)
        warnings++;
}

/** eeprom.c's targets, which the test sets. */
void ath9k_eeprom_target_power(const struct ath9k *card, unsigned freq,
                               uint8_t target[ar9300RateSize])
{
    (void)card;
    (void)freq;
    k_memcpy(target, targets, sizeof(targets));
}

/** eeprom.c's conformance test limits, which the test sets. */
unsigned ath9k_eeprom_ctl_limit(const struct ath9k *card, unsigned freq, unsigned mode)
{
    (void)card;
    (void)freq;
    if (mode == CTL_11B)
        return ctl_11b;
    if (mode == CTL_11G)
        return ctl_11g;
    CHECK(mode == CTL_2GHT20);
    return ctl_ht20;
}

/** recover.c's reset request, recorded. */
void ath9k_reset_schedule(struct ath9k *card, enum ath9k_reset_reason why)
{
    CHECK(card == &ah);
    resets++;
    reset_why = why;
}

/* ---- the chip ---- */

/* The calibration DAC value above which the peak detector's output turns. */
#define PEAK_DAC 40

/** Grant the radio bus while it is asked for, and turn the peak detector's
 * output at PEAK_DAC. */
static int chip_read(struct ath9k_model *m, uint32_t reg, uint32_t *value)
{
    if (reg == AR_PHY_RFBUS_GRANT) {
        *value = !no_grant && (m->reg[AR_PHY_RFBUS_REQ / 4] & AR_PHY_RFBUS_REQ_EN)
                     ? AR_PHY_RFBUS_GRANT_EN
                     : 0;
        return 1;
    }
    if (reg == AR_PHY_65NM_RXRF_AGC(0)) {
        uint32_t v = m->reg[reg / 4] & ~AR_PHY_65NM_RXRF_AGC_AGC_OUT;

        if (MS(v, AR_PHY_65NM_RXRF_AGC_AGC2G_CALDAC_OVR) > PEAK_DAC)
            v |= AR_PHY_65NM_RXRF_AGC_AGC_OUT;
        *value = v;
        return 1;
    }
    return 0;
}

/** Finish the AGC and noise floor calibrations and the RX IQ sample as
 * soon as they are started, unless the test holds them. */
static void chip_write(struct ath9k_model *m, uint32_t reg, uint32_t value)
{
    if (reg == AR_PHY_AGC_CONTROL) {
        if (!stuck_cal)
            value &= ~AR_PHY_AGC_CONTROL_CAL;
        if (!stuck_nf)
            value &= ~AR_PHY_AGC_CONTROL_NF;
        m->reg[reg / 4] = value;
    } else if (reg == AR_PHY_TIMING4 && !keep_do_cal)
        m->reg[reg / 4] = value & ~AR_PHY_TIMING4_DO_CAL;
}

/** A powered card of the G50-70's AR9565 1.0.1 with its tables, nothing
 * queued, and every test knob at rest. */
static void card(void)
{
    wlan_cancel_work(&ah.cal.work[0].work);
    wlan_cancel_work(&ah.cal.work[1].work);
    k_memset(&ah, 0, sizeof(ah));
    ah.hw.chip = ath9k_chip_find(0x0036);
    CHECK(ah.hw.chip != NULL);
    ah.hw.mac_rev = 1;
    ah.hw.chip->ops->init_mode_regs(&ah);
    ath9k_model_init(&model, ah.hw.chip, 1);
    model.hooks.read = chip_read;
    model.hooks.write = chip_write;
    ath9k_model_attach(&model, &ah);
    stuck_cal = stuck_nf = keep_do_cal = no_grant = 0;
    warnings = resets = 0;
    k_memset(targets, 30, sizeof(targets));
    ctl_11b = ctl_11g = ctl_ht20 = 60;
}

/** A 2.4 GHz channel of `freq` MHz with a maximum of `dbm`. */
static struct wlan_channel chan_of(unsigned freq, int dbm)
{
    struct wlan_channel c;

    k_memset(&c, 0, sizeof(c));
    c.frequency = (uint16_t)freq;
    c.number = (uint8_t)(freq == 2484 ? 14 : (freq - 2407) / 5);
    c.max_power = (int8_t)dbm;
    return c;
}

/** The index of the first write to `reg` in the log at or after `from`,
 * or -1. */
static int wr(unsigned from, uint32_t reg)
{
    return ath9k_model_find(&model, from, ATH9K_MODEL_WRITE, reg);
}

/** Field `mask` of the register's current value. */
static uint32_t field(uint32_t reg, uint32_t mask)
{
    uint32_t v = ath9k_model_peek(&model, reg) & mask;

    while (mask && !(mask & 1)) {
        mask >>= 1;
        v >>= 1;
    }
    return v;
}

/* ---- the synthesizer ---- */

/** SYNTH7's value for `freq` in fractional mode from the 40 MHz
 * reference: 3/4 * 40 MHz * (sel + frac / 2^17). */
static uint32_t synth7(unsigned freq)
{
    uint32_t sel = freq * 4 / 120, frac = (freq * 4 % 120) * 0x20000 / 120;

    return (((sel << 17) | frac) << 2) | (1u << 30);
}

static void test_synth(void)
{
    struct wlan_channel c;
    unsigned n, freq;
    int i, j, k;

    card();
    /* Two by hand: 2412 is 80 + 0.4, 2437 is 81 + 0.2333. */
    CHECK(synth7(2412) == 0x42833330u);
    CHECK(synth7(2437) == 0x4289dddcu);
    for (n = 1; n <= 14; n++) {
        freq = n == 14 ? 2484 : 2407 + 5 * n;
        c = chan_of(freq, 20);
        ath9k_model_log_clear(&model);
        CHECK(ath9k_phy_set_channel(&ah, &c) == 0);
        i = wr(0, AR_PHY_SYNTH_CONTROL);
        j = wr(0, AR_PHY_65NM_CH0_SYNTH4);
        k = wr(0, AR_PHY_65NM_CH0_SYNTH7);
        CHECK(i >= 0 && j > i && k > j);
        CHECK(model.log[i].value == 1u << 29);
        CHECK(MS(model.log[j].value, AR_PHY_SYNTH4_LONG_SHIFT_SELECT) == 1);
        CHECK(ath9k_model_count(&model, ATH9K_MODEL_WRITE, AR_PHY_65NM_CH0_SYNTH7) == 2);
        CHECK(k >= 0 && model.log[k].value == synth7(freq));
        CHECK(ath9k_model_last_write(&model, AR_PHY_65NM_CH0_SYNTH7, 0) ==
              (synth7(freq) | 1u << 31));
        CHECK(ah.phy.channel.frequency == freq);
    }
    c = chan_of(2407, 20);
    ath9k_model_log_clear(&model);
    CHECK(ath9k_phy_set_channel(&ah, &c) == -EINVAL);
    c.frequency = 5180;
    CHECK(ath9k_phy_set_channel(&ah, &c) == -EINVAL);
    CHECK(model.log_count == 0 && ah.phy.channel.frequency == 2484);
}

/* ---- the tables ---- */

#define MAX_EXPECT 8192
static uint32_t exp_reg[MAX_EXPECT], exp_val[MAX_EXPECT];
static unsigned n_exp;

/** Expect column `column` of `ini`, or column 1 of a table with one
 * value for every mode, as ath9k's ar9003_hw_prog_ini writes it. */
static void expect_ini(const struct ath9k_ini *ini, unsigned column)
{
    unsigned r;

    if (!ini->words)
        return;
    if (column >= ini->columns)
        column = 1;
    for (r = 0; r < ini->rows && n_exp < MAX_EXPECT; r++, n_exp++) {
        exp_reg[n_exp] = ini->words[r * ini->columns];
        exp_val[n_exp] = ini->words[r * ini->columns + column];
    }
}

/** Check that process_ini writes the tables in ath9k's order, then the
 * driver's overrides, the channel's registers, the chains and the
 * transmit power. */
static void check_process_ini(unsigned freq)
{
    struct wlan_channel c = chan_of(freq, 20);
    static const uint32_t after[] = {
        AR_DIAG_SW,      AR_PCU_MISC_MODE2,     AR_GLB_SWREG_DISCONT_MODE,
        AR_PHY_GEN_CTRL, AR_2040_MODE,          AR_GTXTO,
        AR_CST,          AR_PHY_RX_CHAINMASK,   AR_PHY_CAL_CHAINMASK,
        AR_SELFGEN_MASK, AR_PHY_TX_FORCED_GAIN, AR_PHY_POWER_TX_RATE(0)};
    unsigned i, w, bad = 0;

    card();
    n_exp = 0;
    for (i = 0; i < ATH9K_INI_STAGES; i++) {
        expect_ini(&ah.hw.soc[i], ATH9K_INI_2G_HT20);
        expect_ini(&ah.hw.mac[i], ATH9K_INI_2G_HT20);
        expect_ini(&ah.hw.bb[i], ATH9K_INI_2G_HT20);
        expect_ini(&ah.hw.radio[i], ATH9K_INI_2G_HT20);
    }
    expect_ini(&ah.hw.rx_gain, 1);
    expect_ini(&ah.hw.tx_gain, ATH9K_INI_2G_HT20);
    if (freq == 2484)
        expect_ini(&ah.hw.japan_2484, 1);
    CHECK(n_exp > 500 && n_exp < MAX_EXPECT);
    for (i = 0; i < sizeof(after) / sizeof(after[0]); i++, n_exp++) {
        exp_reg[n_exp] = after[i];
        exp_val[n_exp] = 0;
    }

    ath9k_phy_process_ini(&ah, &c);
    CHECK(model.log_lost == 0 && model.unmapped == 0);
    for (i = 0, w = 0; i < model.log_count && w < n_exp; i++) {
        if (model.log[i].op != ATH9K_MODEL_WRITE)
            continue;
        if (model.log[i].reg != exp_reg[w] || (exp_val[w] && model.log[i].value != exp_val[w]) ||
            (!exp_val[w] && w < n_exp - 12 && model.log[i].value != 0))
            bad++;
        w++;
    }
    CHECK(w == n_exp && bad == 0);

    /* The overrides' values. */
    CHECK(ath9k_model_peek(&model, AR_DIAG_SW) & AR_DIAG_RX_DIS);
    CHECK(ath9k_model_peek(&model, AR_DIAG_SW) & AR_DIAG_RX_ABORT);
    CHECK(!(ath9k_model_peek(&model, AR_PCU_MISC_MODE2) & AR_ADHOC_MCAST_KEYID_ENABLE));
    CHECK(ath9k_model_peek(&model, AR_PCU_MISC_MODE2) & AR_AGG_WEP_ENABLE_FIX);
    CHECK(ath9k_model_peek(&model, AR_GLB_SWREG_DISCONT_MODE) == AR_GLB_SWREG_DISCONT_EN_BT_WLAN);
    CHECK(ath9k_model_peek(&model, AR_PHY_GEN_CTRL) & AR_PHY_GC_HT_EN);
    CHECK(!(ath9k_model_peek(&model, AR_PHY_GEN_CTRL) & AR_PHY_GC_GF_DETECT_EN));
    CHECK(ath9k_model_peek(&model, AR_PHY_RX_CHAINMASK) == 1);
    CHECK(ath9k_model_peek(&model, AR_SELFGEN_MASK) == 1);
    CHECK(ah.cal.enabled & ATH9K_CAL_ENABLE_TXIQ_ON_AGC);
    CHECK(!!(ah.cal.enabled & ATH9K_CAL_ENABLE_TXIQ) ==
          !!(ath9k_model_peek(&model, AR_PHY_TX_IQCAL_CONTROL_0) &
             AR_PHY_TX_IQCAL_CONTROL_0_ENABLE_TXIQ_CAL));
    CHECK(!!(ah.cal.enabled & ATH9K_CAL_ENABLE_TXCL) ==
          !!(ath9k_model_peek(&model, AR_PHY_CL_CAL_CTL) & AR_PHY_CL_CAL_ENABLE));
}

static void test_tables(void)
{
    check_process_ini(2412);
    check_process_ini(2484);
    /* The rest of a reset's radio part. */
    card();
    {
        struct wlan_channel c = chan_of(2412, 20);

        ath9k_phy_set_rfmode(&ah, &c);
        CHECK(ath9k_model_last_write(&model, AR_PHY_MODE, 0) == AR_PHY_MODE_DYNAMIC);
        ath9k_phy_set_delta_slope(&ah, &c);
        CHECK(field(AR_PHY_TIMING3, AR_PHY_TIMING3_DSC_MAN) == 21737);
        CHECK(field(AR_PHY_TIMING3, AR_PHY_TIMING3_DSC_EXP) == 3);
        CHECK(field(AR_PHY_SGI_DELTA, AR_PHY_SGI_DSC_MAN) == 19563);
        CHECK(field(AR_PHY_SGI_DELTA, AR_PHY_SGI_DSC_EXP) == 3);
        ath9k_model_poke(&model, AR_PHY_RX_DELAY, 2000);
        ath9k_model_log_clear(&model);
        ath9k_phy_init_bb(&ah, &c);
        CHECK(model.log_count == 2 && model.log[0].reg == AR_PHY_ACTIVE &&
              model.log[0].value == AR_PHY_ACTIVE_EN);
        CHECK(model.log[1].op == ATH9K_MODEL_DELAY && model.log[1].value == 200 + 100);
        ath9k_phy_mark_inactive(&ah);
        CHECK(ath9k_model_last_write(&model, AR_PHY_ACTIVE, 0) == AR_PHY_ACTIVE_DIS);
    }
}

/* ---- the spurs ---- */

/** Mitigate the spurs at bins `a` and `b` on `freq`. */
static void spur_run(unsigned freq, uint8_t a, uint8_t b)
{
    struct wlan_channel c = chan_of(freq, 20);

    card();
    ath9k_phy_set_rfmode(&ah, &c);
    ah.eeprom.image.modalHeader2G.spurChans[0] = a;
    ah.eeprom.image.modalHeader2G.spurChans[1] = b;
    ath9k_model_log_clear(&model);
    ath9k_phy_spur_mitigate(&ah, &c);
}

static void test_spur(void)
{
    /* None listed: nothing is touched. */
    spur_run(2412, 0, 0);
    CHECK(model.log_count == 0);

    /* 2440 and 2445 MHz on channel 6: offsets 3 and 8, masks A and B, and
     * no self-correlation filter at 2437. */
    spur_run(2437, 140, 145);
    CHECK(field(AR_PHY_TIMING4, AR_PHY_TIMING4_ENABLE_SPUR_FILTER) == 1);
    CHECK(field(AR_PHY_TIMING11, AR_PHY_TIMING11_SPUR_FREQ_SD) == 3 * 512 / 11);
    CHECK(field(AR_PHY_TIMING11, AR_PHY_TIMING11_SPUR_DELTA_PHASE) == 3 * 262144 / 5);
    CHECK(field(AR_PHY_TIMING11, AR_PHY_TIMING11_USE_SPUR_FILTER_IN_AGC) == 1);
    CHECK(field(AR_PHY_TIMING11, AR_PHY_TIMING11_USE_SPUR_FILTER_IN_SELFCOR) == 0);
    CHECK(field(AR_PHY_SPUR_REG, AR_PHY_SPUR_REG_ENABLE_NF_RSSI_SPUR_MIT) == 1);
    CHECK(field(AR_PHY_PILOT_SPUR_MASK, AR_PHY_PILOT_SPUR_MASK_CF_PILOT_MASK_IDX_A) == 9);
    CHECK(field(AR_PHY_SPUR_MASK_A, AR_PHY_SPUR_MASK_A_CF_PUNC_MASK_IDX_A) == 9);
    CHECK(field(AR_PHY_CHAN_SPUR_MASK, AR_PHY_CHAN_SPUR_MASK_CF_CHAN_MASK_IDX_A) == 9);
    CHECK(field(AR_PHY_SPUR_MASK_A, AR_PHY_SPUR_MASK_A_CF_PUNC_MASK_A) == 0xa0);
    CHECK(field(AR_PHY_PILOT_SPUR_MASK, AR_PHY_PILOT_SPUR_MASK_CF_PILOT_MASK_IDX_B) == 25);
    CHECK(field(AR_PHY_SPUR_MASK_B, AR_PHY_SPUR_MASK_A_CF_PUNC_MASK_IDX_A) == 25);
    CHECK(field(AR_PHY_CHAN_SPUR_MASK, AR_PHY_CHAN_SPUR_MASK_CF_CHAN_MASK_IDX_B) == 25);
    CHECK(field(AR_PHY_CHAN_SPUR_MASK, AR_PHY_CHAN_SPUR_MASK_CF_CHAN_MASK_B) == 0xe);
    CHECK(field(AR_PHY_SPUR_MASK_B, AR_PHY_SPUR_MASK_A_CF_PUNC_MASK_A) == 0xa0);

    /* 2405 MHz on channel 1: offset -7, the negative index rounded down,
     * the self-correlation filter on, and no second spur near. */
    spur_run(2412, 105, 0);
    CHECK(field(AR_PHY_TIMING11, AR_PHY_TIMING11_SPUR_FREQ_SD) == (uint32_t)(1024 - 325));
    CHECK(field(AR_PHY_TIMING11, AR_PHY_TIMING11_SPUR_DELTA_PHASE) ==
          (uint32_t)(0x100000 - 367001));
    CHECK(field(AR_PHY_TIMING11, AR_PHY_TIMING11_USE_SPUR_FILTER_IN_SELFCOR) == 1);
    CHECK(field(AR_PHY_PILOT_SPUR_MASK, AR_PHY_PILOT_SPUR_MASK_CF_PILOT_MASK_IDX_A) == 105);
    CHECK(ath9k_model_count(&model, ATH9K_MODEL_WRITE, AR_PHY_SPUR_MASK_B) == 0);

    /* 2462 MHz is out of channel 1's reach: the fields are cleared. */
    spur_run(2412, 162, 0);
    CHECK(model.log_count > 0);
    CHECK(field(AR_PHY_TIMING4, AR_PHY_TIMING4_ENABLE_SPUR_FILTER) == 0);
    CHECK(field(AR_PHY_SPUR_REG, AR_PHY_SPUR_REG_MASK_RATE_CNTL) == 0);
}

/* ---- the transmit power ---- */

/** Give the EEPROM three calibration piers at 2412, 2437 and 2472 MHz. */
static void piers(void)
{
    struct ar9300_eeprom *e = &ah.eeprom.image;
    static const int8_t ref[3] = {2, 4, -2};
    static const uint8_t temp[3] = {120, 130, 140};
    unsigned i;

    e->calFreqPier2G[0] = 112;
    e->calFreqPier2G[1] = 137;
    e->calFreqPier2G[2] = 172;
    for (i = 0; i < 3; i++) {
        e->calPierData2G[0][i].refPower = ref[i];
        e->calPierData2G[0][i].tempMeas = temp[i];
        e->calPierData2G[0][i].rxTempMeas = 1;
        e->calPierData2G[0][i].rxNoisefloorCal = -25;
    }
    e->modalHeader2G.tempSlope = 25;
    e->modalHeader2G.antennaGain = 4;
}

/** Four powers as AR_PHY_POWER_TX_RATE holds them. */
static uint32_t pow4(unsigned a, unsigned b, unsigned c, unsigned d)
{
    return a | b << 8 | c << 16 | (uint32_t)d << 24;
}

static void test_txpower(void)
{
    struct wlan_channel c = chan_of(2412, 20);
    unsigned i;

    card();
    piers();
    for (i = 0; i < ar9300RateSize; i++)
        targets[i] = 30;
    targets[ALL_TARGET_LEGACY_6_24] = 34;
    targets[ALL_TARGET_LEGACY_54] = 28;
    targets[ALL_TARGET_LEGACY_1L_5L] = 38;
    targets[ALL_TARGET_HT20_0_8_16] = 40;
    targets[ALL_TARGET_HT40_0_8_16] = 50;

    /* No limit: the channel's 20 dBm less the antenna's 2 dB is 36. */
    ath9k_model_log_clear(&model);
    ath9k_phy_set_txpower(&ah, &c, 0);
    CHECK(ah.phy.power_limit == 0);
    CHECK(ath9k_model_peek(&model, AR_PHY_POWER_TX_RATE(0)) == pow4(34, 34, 34, 34));
    CHECK(ath9k_model_peek(&model, AR_PHY_POWER_TX_RATE(1)) == pow4(34, 30, 30, 28));
    CHECK(ath9k_model_peek(&model, AR_PHY_POWER_TX_RATE(2)) == pow4(36, 0, 36, 36));
    CHECK(ath9k_model_peek(&model, AR_PHY_POWER_TX_RATE(4)) == pow4(36, 30, 30, 30));
    /* HT40 is not capped: no row has it. */
    CHECK(ath9k_model_peek(&model, AR_PHY_POWER_TX_RATE(6)) == pow4(50, 30, 30, 30));
    CHECK(ath9k_model_peek(&model, AR_TPC) == (SM(36, AR_TPC_ACK) | SM(36, AR_TPC_CTS) |
                                               SM(0x3f, AR_TPC_CHIRP) | SM(0x3f, AR_TPC_RPT)));
    CHECK(ath9k_model_last_write(&model, AR_PHY_PWRTX_MAX, 1) == 0);
    CHECK(ah.phy.target[ALL_TARGET_HT20_0_8_16] == 36 &&
          ah.phy.target[ALL_TARGET_HT40_0_8_16] == 50);
    CHECK(wr(0, AR_PHY_TX_FORCED_GAIN) < wr(0, AR_PHY_POWER_TX_RATE(0)));
    CHECK(wr(0, AR_PHY_POWER_TX_RATE(11)) < wr(0, AR_PHY_TPC_11_B0));
    CHECK(wr(0, AR_PHY_TPC_18) < wr(0, AR_TPC));
    /* The pier at the channel: its correction, temperature and floor. */
    CHECK(field(AR_PHY_TPC_11_B0, AR_PHY_TPC_OLPC_GAIN_DELTA) == 2);
    CHECK(field(AR_PHY_TPC_6_B0, AR_PHY_TPC_6_ERROR_EST_MODE) == 3);
    CHECK(field(AR_PHY_TPC_19, AR_PHY_TPC_19_ALPHA_THERM) == 25);
    CHECK(field(AR_PHY_TPC_18, AR_PHY_TPC_18_THERM_CAL_VALUE) == 120);
    CHECK(ah.phy.nf_cal[0] == -96);
    /* One chain: the other chains' registers are not written. */
    CHECK(ath9k_model_count(&model, ATH9K_MODEL_WRITE, AR_PHY_TPC_11_B1) == 0);

    /* The caller's 15 dBm. */
    ath9k_phy_set_txpower(&ah, &c, 30);
    CHECK(ah.phy.power_limit == 30);
    CHECK(ath9k_model_peek(&model, AR_PHY_POWER_TX_RATE(0)) == pow4(26, 26, 26, 26));
    CHECK(ah.phy.target[ALL_TARGET_LEGACY_1L_5L] == 26);

    /* A channel of 10 dBm. */
    c.max_power = 10;
    ath9k_phy_set_txpower(&ah, &c, 0);
    CHECK(ah.phy.target[ALL_TARGET_LEGACY_6_24] == 16);
    c.max_power = 20;

    /* The CCK conformance limit, alone. */
    ctl_11b = 20;
    ath9k_phy_set_txpower(&ah, &c, 0);
    CHECK(ath9k_model_peek(&model, AR_PHY_POWER_TX_RATE(3)) == pow4(20, 20, 20, 20));
    CHECK(ah.phy.target[ALL_TARGET_LEGACY_6_24] == 34);
    CHECK(ah.phy.target[ALL_TARGET_HT20_0_8_16] == 36);
    ctl_11b = 60;
    ctl_ht20 = 24;
    ath9k_phy_set_txpower(&ah, &c, 0);
    CHECK(ah.phy.target[ALL_TARGET_HT20_0_8_16] == 24 && ah.phy.target[ALL_TARGET_HT20_23] == 24);
    CHECK(ah.phy.target[ALL_TARGET_LEGACY_1L_5L] == 36);
    ctl_ht20 = 60;

    /* A negative antenna gain takes nothing off. */
    ah.eeprom.image.modalHeader2G.antennaGain = -6;
    ath9k_phy_set_txpower(&ah, &c, 0);
    CHECK(ah.phy.target[ALL_TARGET_HT20_0_8_16] == 40);
    ah.eeprom.image.modalHeader2G.antennaGain = 4;

    /* Between piers: 2422 MHz interpolates 2 and 4 to 3, 120 and 130 to
     * 124. */
    c = chan_of(2422, 20);
    ath9k_phy_set_txpower(&ah, &c, 0);
    CHECK(field(AR_PHY_TPC_11_B0, AR_PHY_TPC_OLPC_GAIN_DELTA) == 3);
    CHECK(field(AR_PHY_TPC_18, AR_PHY_TPC_18_THERM_CAL_VALUE) == 124);

    /* 2467 MHz: a correction of -1, which stays inside its field. */
    ath9k_model_poke(&model, AR_PHY_TPC_11_B0, 0x80000001u);
    c = chan_of(2467, 20);
    ath9k_phy_set_txpower(&ah, &c, 0);
    CHECK(field(AR_PHY_TPC_11_B0, AR_PHY_TPC_OLPC_GAIN_DELTA) ==
          (AR_PHY_TPC_OLPC_GAIN_DELTA >> AR_PHY_TPC_OLPC_GAIN_DELTA_S));
    CHECK((ath9k_model_peek(&model, AR_PHY_TPC_11_B0) & ~AR_PHY_TPC_OLPC_GAIN_DELTA) ==
          (0x80000001u & ~AR_PHY_TPC_OLPC_GAIN_DELTA));
}

/* ---- the reset-time calibrations ---- */

static void test_cal_init(void)
{
    struct wlan_channel c = chan_of(2412, 20);
    int sel, iq1, iq0, agc, peak, mode;

    card();
    ath9k_phy_process_ini(&ah, &c);
    ath9k_cal_set_channel(&ah, &c);
    ath9k_model_log_clear(&model);
    CHECK(ath9k_cal_init(&ah, &c) == 0);
    sel = wr(0, AR_SELFGEN_MASK);
    iq1 = wr(0, AR_PHY_TX_IQCAL_CONTROL_1);
    iq0 = wr(0, AR_PHY_TX_IQCAL_CONTROL_0);
    agc = wr(0, AR_PHY_AGC_CONTROL);
    peak = wr(0, AR_PHY_65NM_RXRF_GAINSTAGES(0));
    mode = wr(0, AR_PHY_CALMODE);
    CHECK(sel >= 0 && iq1 > sel && iq0 > iq1 && agc > iq0 && peak > agc && mode > peak);
    CHECK(agc >= 0 && (model.log[agc].value & AR_PHY_AGC_CONTROL_CAL));
    CHECK(field(AR_PHY_TX_IQCAL_CONTROL_1, AR_PHY_TX_IQCAL_CONTROL_1_IQCORR_I_Q_COFF_DELPT) == 32);
    CHECK(field(AR_PHY_TX_IQCAL_CONTROL_0, AR_PHY_TX_IQCAL_CONTROL_0_ENABLE_TXIQ_CAL) == 1);
    /* The peak detector's DAC, found bit by bit, is the last below the
     * turn; the overrides are released. */
    CHECK(field(AR_PHY_65NM_RXRF_AGC(0), AR_PHY_65NM_RXRF_AGC_AGC2G_CALDAC_OVR) == PEAK_DAC);
    CHECK(field(AR_PHY_65NM_RXRF_AGC(0), AR_PHY_65NM_RXRF_AGC_AGC_CAL_OVR) == 0);
    CHECK(field(AR_PHY_65NM_RXRF_GAINSTAGES(0), AR_PHY_65NM_RXRF_GAINSTAGES_RX_OVERRIDE) == 0);
    CHECK(field(AR_PHY_65NM_RXTX2(0), AR_PHY_65NM_RXTX2_RXON_OVR) == 0);
    CHECK(ath9k_model_count(&model, ATH9K_MODEL_WRITE, AR_PHY_65NM_RXRF_GAINSTAGES(1)) == 0);
    /* The TX IQ result is kept for the channel, and the RX IQ calibration
     * starts. */
    CHECK(ah.cal.flags & ATH9K_CAL_TXIQ_DONE);
    CHECK(field(AR_PHY_TX_IQCAL_CONTROL_3, AR_PHY_TX_IQCAL_CONTROL_3_IQCORR_EN) == 1);
    CHECK(ah.cal.iq_state == ATH9K_CAL_RUNNING && !ah.cal.iq_valid);
    CHECK(ath9k_model_last_write(&model, AR_PHY_CALMODE, 0) == AR_PHY_CALMODE_IQ);
    CHECK(warnings == 0);

    /* Again on the same channel: the TX IQ calibration is not run. */
    ath9k_model_log_clear(&model);
    CHECK(ath9k_cal_init(&ah, &c) == 0);
    CHECK(field(AR_PHY_TX_IQCAL_CONTROL_0, AR_PHY_TX_IQCAL_CONTROL_0_ENABLE_TXIQ_CAL) == 0);
    /* A new channel forgets it. */
    c = chan_of(2437, 20);
    ath9k_cal_set_channel(&ah, &c);
    CHECK(!(ah.cal.flags & ATH9K_CAL_TXIQ_DONE) && ah.cal.freq == 2437);

    /* The offset calibration never finishes. */
    stuck_cal = 1;
    CHECK(ath9k_cal_init(&ah, &c) == -EIO);
    CHECK(warnings == 1);
    CHECK(!(ah.cal.flags & ATH9K_CAL_TXIQ_DONE));
}

/* ---- the noise floor ---- */

/** Put a reading of `dbm` in chain 0's CCA register. */
static void cca(int dbm)
{
    uint32_t v = ath9k_model_peek(&model, AR_PHY_CCA_0) & ~0x1ff00000u;

    ath9k_model_poke(&model, AR_PHY_CCA_0, v | (((uint32_t)dbm & 0x1ff) << 20));
}

static void test_nf(void)
{
    struct wlan_channel c = chan_of(2412, 20);
    static const int readings[] = {-100, -90, -80, -95, -85};
    unsigned i;
    int j;

    card();
    ah.phy.nf_cal[0] = -112;
    ath9k_cal_set_channel(&ah, &c);
    CHECK(ah.cal.nf[0].private_nf == -112 && ah.cal.nf[0].invalid_count == 5);
    /* No measurement before the first tune. */
    CHECK(ath9k_nf_read(&ah) == 0);
    CHECK(ath9k_phy_set_channel(&ah, &c) == 0);

    /* Still measuring. */
    ath9k_model_poke(&model, AR_PHY_AGC_CONTROL, AR_PHY_AGC_CONTROL_NF);
    CHECK(ath9k_nf_read(&ah) == 0);
    ath9k_model_poke(&model, AR_PHY_AGC_CONTROL, 0);

    /* A new history takes its first five readings whole. */
    for (i = 0; i < 5; i++) {
        cca(readings[i]);
        CHECK(ath9k_nf_read(&ah) == 1);
        CHECK(ah.cal.nf[0].private_nf == readings[i]);
    }
    CHECK(ah.cal.noise_floor == -85);
    CHECK(ah.cal.noise == -95 + (-85 - 6 + 112));
    /* Then the median of the last five: -70 -80 -85 -90 -95. */
    cca(-70);
    CHECK(ath9k_nf_read(&ah) == 1);
    CHECK(ah.cal.nf[0].private_nf == -85);
    /* Readings outside the bounds: above the maximum to it. */
    cca(-40);
    CHECK(ath9k_nf_read(&ah) == 1);
    CHECK(ah.cal.nf[0].buf[(ah.cal.nf[0].index + 4) % 5] == -60);
    CHECK(ah.cal.nf[0].private_nf == -80);

    /* The load: the median, a load started and finished, the release. */
    ah.cal.nf[0].private_nf = -85;
    ath9k_model_log_clear(&model);
    CHECK(ath9k_nf_load(&ah) == 0);
    j = wr(0, AR_PHY_CCA_0);
    CHECK(j == 0 && (model.log[0].value & 0x1ff) == ((uint32_t)-170 & 0x1ff));
    CHECK(wr(0, AR_PHY_AGC_CONTROL) > j);
    CHECK(ath9k_model_count(&model, ATH9K_MODEL_WRITE, AR_PHY_CCA_0) == 2);
    CHECK((ath9k_model_peek(&model, AR_PHY_CCA_0) & 0x1ff) == ((uint32_t)-100 & 0x1ff));
    CHECK(ath9k_model_count(&model, ATH9K_MODEL_WRITE, AR_PHY_EXT_CCA) == 0);

    /* A load the chip never finishes leaves the median in place. */
    stuck_nf = 1;
    CHECK(ath9k_nf_load(&ah) == -ETIMEDOUT);
    CHECK((ath9k_model_peek(&model, AR_PHY_CCA_0) & 0x1ff) == ((uint32_t)-170 & 0x1ff));
    stuck_nf = 0;

    /* A measurement the baseband takes itself. */
    ath9k_model_log_clear(&model);
    stuck_nf = 1;
    ath9k_nf_start(&ah);
    CHECK(ah.cal.flags & ATH9K_CAL_NF_PENDING);
    CHECK(ath9k_model_peek(&model, AR_PHY_AGC_CONTROL) & AR_PHY_AGC_CONTROL_ENABLE_NF);
    CHECK(!(ath9k_model_peek(&model, AR_PHY_AGC_CONTROL) & AR_PHY_AGC_CONTROL_NO_UPDATE_NF));
    CHECK(ath9k_model_peek(&model, AR_PHY_AGC_CONTROL) & AR_PHY_AGC_CONTROL_NF);
    /* Stuck beacons while it runs: interference is assumed. */
    ath9k_nf_bstuck(&ah);
    CHECK(ah.cal.flags & ATH9K_CAL_NF_INTF);
    CHECK(ah.cal.flags & ATH9K_CAL_NF_PENDING);
    stuck_nf = 0;
    ath9k_model_poke(&model, AR_PHY_AGC_CONTROL, 0);
    cca(-90);
    ath9k_nf_bstuck(&ah);
    CHECK(!(ah.cal.flags & ATH9K_CAL_NF_PENDING));
    /* A normal reading ends the interference. */
    CHECK(ath9k_nf_read(&ah) == 1);
    CHECK(!(ah.cal.flags & ATH9K_CAL_NF_INTF));
}

/* ---- the periodic work and the RX IQ calibration ---- */

/** Give the RX IQ calibration sums of one sample. */
static void iq_meas(uint32_t pi, uint32_t pq, uint32_t iq)
{
    ath9k_model_poke(&model, AR_PHY_CAL_MEAS_0(0), pi);
    ath9k_model_poke(&model, AR_PHY_CAL_MEAS_1(0), pq);
    ath9k_model_poke(&model, AR_PHY_CAL_MEAS_2(0), iq);
}

/** A card tuned to 2412 MHz after a reset's calibrations, its work
 * started at `now`. */
static void card_running(void)
{
    struct wlan_channel c = chan_of(2412, 20);

    card();
    ath9k_cal_set_channel(&ah, &c);
    ath9k_phy_process_ini(&ah, &c);
    CHECK(ath9k_phy_set_channel(&ah, &c) == 0);
    ath9k_ani_reset(&ah);
    CHECK(ath9k_cal_init(&ah, &c) == 0);
    ath9k_cal_start(&ah);
}

static void test_work(void)
{
    uint64_t t;

    /* A normal sample: I 1/256 of the power, Q balanced. */
    card_running();
    t = now;
    CHECK(wlan_work_next_due() == t + 1000);
    iq_meas(0x10000, 0x10000, 0x100);
    ath9k_model_log_clear(&model);
    now = t + 999;
    CHECK(wlan_work_run_due(now) == 0);
    now = t + 1000;
    CHECK(wlan_work_run_due(now) == 1);
    CHECK(field(AR_PHY_RX_IQCAL_CORR_B0, AR_PHY_RX_IQCAL_CORR_IQCORR_Q_I_COFF) == 0x7f);
    CHECK(field(AR_PHY_RX_IQCAL_CORR_B0, AR_PHY_RX_IQCAL_CORR_IQCORR_Q_Q_COFF) == 0);
    CHECK(field(AR_PHY_RX_IQCAL_CORR_B0, AR_PHY_RX_IQCAL_CORR_IQCORR_ENABLE) == 1);
    CHECK(ah.cal.done && ah.cal.iq_valid && ah.cal.iq_state == ATH9K_CAL_DONE);
    CHECK(wlan_work_next_due() == t + 2000);
    /* Done: no further sample. */
    ath9k_model_log_clear(&model);
    now = t + 2000;
    CHECK(wlan_work_run_due(now) == 1);
    CHECK(ath9k_model_count(&model, ATH9K_MODEL_WRITE, AR_PHY_CALMODE) == 0);
    /* The thermometer is read. */
    ath9k_model_poke(&model, AR_PHY_BB_THERM_ADC_4,
                     SM(0x55, AR_PHY_BB_THERM_ADC_4_LATEST_THERM_VALUE));
    now = t + 3000;
    CHECK(wlan_work_run_due(now) == 1);
    CHECK(ah.cal.temp == 0x55);
    /* Twenty minutes on, it starts again, and the next second's sample
     * finishes it. */
    now = t + 1000 + ATH9K_RESTART_CAL_MS;
    CHECK(wlan_work_run_due(now) == 1);
    CHECK(!ah.cal.done && !ah.cal.iq_valid);
    now += 1000;
    CHECK(wlan_work_run_due(now) == 1);
    CHECK(ah.cal.done && ah.cal.iq_valid);
    ath9k_cal_stop(&ah);
    CHECK(wlan_work_next_due() == 0);

    /* A coefficient at its bound: nothing is written, not even the enable
     * (6f37ff96d3bd). */
    card_running();
    t = now;
    iq_meas(0x10000, 0x10000, 0x10000);
    ath9k_model_log_clear(&model);
    now = t + 1000;
    CHECK(wlan_work_run_due(now) == 1);
    CHECK(ath9k_model_count(&model, ATH9K_MODEL_WRITE, AR_PHY_RX_IQCAL_CORR_B0) == 0);
    CHECK(ah.cal.done);
    /* And the other bound. */
    card_running();
    t = now;
    iq_meas(0x10000, 0x10000, 0xffff0000u);
    ath9k_model_log_clear(&model);
    now = t + 1000;
    CHECK(wlan_work_run_due(now) == 1);
    CHECK(ath9k_model_count(&model, ATH9K_MODEL_WRITE, AR_PHY_RX_IQCAL_CORR_B0) == 0);
    ath9k_cal_stop(&ah);

    /* A sample still running waits. */
    card_running();
    t = now;
    keep_do_cal = 1;
    ath9k_model_poke(&model, AR_PHY_TIMING4,
                     ath9k_model_peek(&model, AR_PHY_TIMING4) | AR_PHY_TIMING4_DO_CAL);
    now = t + 1000;
    CHECK(wlan_work_run_due(now) == 1);
    CHECK(!ah.cal.done && ah.cal.samples == 0);
    ath9k_cal_stop(&ah);
    keep_do_cal = 0;

    /* The long calibration loads the floor; a load the chip refuses asks
     * for a reset and queues nothing (7b8aaead958e). */
    card_running();
    t = now;
    ath9k_model_poke(&model, AR_PHY_AGC_CONTROL, 0);
    cca(-90);
    for (now = t + 1000; now < t + ATH9K_LONG_CAL_MS; now += 1000)
        CHECK(wlan_work_run_due(now) == 1);
    ath9k_model_log_clear(&model);
    CHECK(wlan_work_run_due(now) == 1);
    CHECK(wr(0, AR_PHY_CCA_0) >= 0 && resets == 0);
    CHECK(ah.cal.flags & ATH9K_CAL_NF_PENDING);
    CHECK(ath9k_model_peek(&model, AR_PHY_AGC_CONTROL) & AR_PHY_AGC_CONTROL_NO_UPDATE_NF);
    ath9k_model_poke(&model, AR_PHY_AGC_CONTROL, 0);
    stuck_nf = 1;
    t = now;
    for (now = t + 1000; now < t + ATH9K_LONG_CAL_MS; now += 1000)
        CHECK(wlan_work_run_due(now) == 1);
    CHECK(resets == 0);
    CHECK(wlan_work_run_due(now) == 1);
    CHECK(resets == 1 && reset_why == ATH9K_RESET_CAL && warnings == 1);
    CHECK(wlan_work_next_due() == 0 && !ah.cal.done);
    stuck_nf = 0;
}

/* ---- noise immunity ---- */

/** The MIB counters of `ms` milliseconds of listening with `ofdm` and
 * `cck` errors. */
static void mib(unsigned ms, unsigned ofdm, unsigned cck)
{
    ath9k_model_poke(&model, AR_CCCNT, ms * 44000);
    ath9k_model_poke(&model, AR_RFCNT, 0);
    ath9k_model_poke(&model, AR_TFCNT, 0);
    ath9k_model_poke(&model, AR_PHY_ERR_1, ofdm);
    ath9k_model_poke(&model, AR_PHY_ERR_2, cck);
}

static void test_ani(void)
{
    static const uint32_t moved[] = {AR_PHY_TIMING5,      AR_PHY_EXT_CCA,     AR_PHY_FIND_SIG,
                                     AR_PHY_FIND_SIG_LOW, AR_PHY_SFCORR,      AR_PHY_SFCORR_LOW,
                                     AR_PHY_SFCORR_EXT,   AR_PHY_MRC_CCK_CTRL};
    struct wlan_channel c = chan_of(2412, 20);
    uint32_t cyc, fir;
    unsigned i;

    card();
    ath9k_phy_process_ini(&ah, &c);
    cyc = field(AR_PHY_TIMING5, AR_PHY_TIMING5_CYCPWR_THR1);
    fir = field(AR_PHY_FIND_SIG, AR_PHY_FIND_SIG_FIRSTEP);
    CHECK(cyc + 2 <= 22 && fir + 2 <= 20);

    /* The defaults are the tables' settings: nothing moves. */
    ath9k_model_log_clear(&model);
    ath9k_ani_reset(&ah);
    for (i = 0; i < sizeof(moved) / sizeof(moved[0]); i++)
        CHECK(ath9k_model_count(&model, ATH9K_MODEL_WRITE, moved[i]) == 0);
    CHECK(ah.ani.ready && ah.ani.ofdm_level == 3 && ah.ani.cck_level == 2);
    CHECK(ath9k_model_last_write(&model, AR_PHY_ERR_1, 1) == 0);
    CHECK(ath9k_model_last_write(&model, AR_PHY_ERR_MASK_1, 0) == AR_PHY_ERR_OFDM_TIMING);
    CHECK(wr(0, AR_FILT_OFDM) >= 0 && wr(0, AR_MIBC) >= 0);

    /* Too short a time to judge: the time adds up, the counts stay. */
    mib(100, 3000, 0);
    ath9k_ani_monitor(&ah);
    CHECK(ah.ani.listen_time == 100 && ah.ani.ofdm_level == 3);
    CHECK(ath9k_model_last_write(&model, AR_CCCNT, 1) == 0);
    /* 3000 OFDM errors in 400 ms: OFDM immunity rises one level. */
    ath9k_model_log_clear(&model);
    mib(300, 3000, 0);
    ath9k_ani_monitor(&ah);
    CHECK(ah.ani.ofdm_level == 4 && ah.ani.listen_time == 0);
    CHECK(field(AR_PHY_TIMING5, AR_PHY_TIMING5_CYCPWR_THR1) == cyc + 2);
    CHECK(field(AR_PHY_FIND_SIG, AR_PHY_FIND_SIG_FIRSTEP) == fir + 2);
    CHECK(ath9k_model_count(&model, ATH9K_MODEL_WRITE, AR_PHY_SFCORR) == 0);
    CHECK(ath9k_model_last_write(&model, AR_PHY_ERR_1, 1) == 0);
    /* A quiet channel lowers CCK's turn, then OFDM's. */
    mib(400, 0, 0);
    ath9k_ani_monitor(&ah);
    CHECK(ah.ani.ofdm_level == 4 && ah.ani.cck_level == 1);
    mib(400, 0, 0);
    ath9k_ani_monitor(&ah);
    CHECK(ah.ani.ofdm_level == 3 && ah.ani.cck_level == 1);
    CHECK(field(AR_PHY_TIMING5, AR_PHY_TIMING5_CYCPWR_THR1) == cyc);
    CHECK(field(AR_PHY_FIND_SIG, AR_PHY_FIND_SIG_FIRSTEP) == fir);
    /* CCK errors raise CCK's level. */
    mib(400, 0, 400);
    ath9k_ani_monitor(&ah);
    CHECK(ah.ani.cck_level == 2 && ah.ani.ofdm_level == 3);
    /* No time heard: the counts restart and nothing moves. */
    mib(0, 3000, 0);
    ath9k_ani_monitor(&ah);
    CHECK(ah.ani.ofdm_level == 3 && ah.ani.listen_time == 0);

    /* A reset, which writes the tables again, keeps the held level. */
    mib(400, 3000, 0);
    ath9k_ani_monitor(&ah);
    CHECK(ah.ani.ofdm_level == 4);
    ath9k_phy_process_ini(&ah, &c);
    CHECK(field(AR_PHY_TIMING5, AR_PHY_TIMING5_CYCPWR_THR1) == cyc);
    ath9k_ani_reset(&ah);
    CHECK(ah.ani.ofdm_level == 4);
    CHECK(field(AR_PHY_TIMING5, AR_PHY_TIMING5_CYCPWR_THR1) == cyc + 2);

    /* Turned off, it reads nothing. */
    ah.options = ATH9K_OPT_NO_ANI;
    model.log_reads = 1;
    ath9k_model_log_clear(&model);
    ath9k_ani_monitor(&ah);
    CHECK(model.log_count == 0);
    model.log_reads = 0;

    /* The boot line turns it off at the first reset. */
    card();
    CHECK(kcmdline_capture("rach2.ath9k_noani") == 0);
    ath9k_phy_process_ini(&ah, &c);
    ath9k_ani_reset(&ah);
    CHECK(ah.options & ATH9K_OPT_NO_ANI);
    mib(400, 3000, 0);
    ath9k_ani_monitor(&ah);
    CHECK(ah.ani.ofdm_level == 3);
    CHECK(kcmdline_capture(0) == 0);
}

/* ---- a channel change ---- */

static void test_channel_change(void)
{
    struct wlan_channel c = chan_of(2412, 20), d = chan_of(2437, 20);
    int req, syn, pow, slope, act, done, load;

    card_running();
    ath9k_cal_stop(&ah);
    ah.phy.power_limit = 30;

    /* A frame in a queue. */
    REG_WRITE(&ah, AR_QTXDP(1), 0x1000);
    ath9k_model_log_clear(&model);
    CHECK(ath9k_phy_channel_change(&ah, &d) == -EBUSY);
    CHECK(model.log_count == 0 && ah.phy.channel.frequency == 2412);
    REG_WRITE(&ah, AR_Q_TXD, 1u << 1);
    REG_WRITE(&ah, AR_Q_TXD, 0);

    /* The radio bus never granted. */
    no_grant = 1;
    ath9k_model_log_clear(&model);
    CHECK(ath9k_phy_channel_change(&ah, &d) == -ETIMEDOUT);
    CHECK(wr(0, AR_PHY_65NM_CH0_SYNTH7) < 0 && ah.phy.channel.frequency == 2412);
    CHECK(ah.cal.freq == 2412);
    no_grant = 0;
    REG_WRITE(&ah, AR_PHY_RFBUS_REQ, 0);

    /* Retuned. */
    ath9k_model_log_clear(&model);
    CHECK(ath9k_phy_channel_change(&ah, &d) == 0);
    req = wr(0, AR_PHY_RFBUS_REQ);
    syn = wr(0, AR_PHY_65NM_CH0_SYNTH7);
    pow = wr(0, AR_PHY_POWER_TX_RATE(0));
    slope = wr(0, AR_PHY_TIMING3);
    act = wr(0, AR_PHY_ACTIVE);
    done = req >= 0 ? wr((unsigned)req + 1, AR_PHY_RFBUS_REQ) : -1;
    load = wr(0, AR_PHY_CCA_0);
    CHECK(req >= 0 && syn > req && pow > syn && slope > pow && act > slope && done > act &&
          load > done);
    CHECK(model.log[req].value == AR_PHY_RFBUS_REQ_EN && model.log[done].value == 0);
    CHECK(ath9k_model_last_write(&model, AR_PHY_65NM_CH0_SYNTH7, 0) == (synth7(2437) | 1u << 31));
    CHECK(ah.phy.channel.frequency == 2437 && ah.cal.freq == 2437 && ah.phy.power_limit == 30);
    CHECK(ah.cal.flags & ATH9K_CAL_NF_PENDING);
    /* No table is written again. */
    CHECK(ath9k_model_count(&model, ATH9K_MODEL_WRITE, AR_PHY_MODE) == 0);

    /* Out of the band: the bus is given back. */
    c.frequency = 5180;
    ath9k_model_log_clear(&model);
    CHECK(ath9k_phy_channel_change(&ah, &c) == -EINVAL);
    CHECK(ath9k_model_last_write(&model, AR_PHY_RFBUS_REQ, 1) == 0);
    CHECK(ah.phy.channel.frequency == 2437 && ah.cal.freq == 2437);
}

int main(void)
{
    (void)kcmdline_capture(0);
    test_synth();
    test_tables();
    test_spur();
    test_txpower();
    test_cal_init();
    test_nf();
    test_work();
    test_ani();
    test_channel_change();
    if (fails) {
        fprintf(stderr, "ath9k_phy_hosttest: %u failed\n", fails);
        return 1;
    }
    printf("ath9k_phy_hosttest: ok\n");
    return 0;
}
