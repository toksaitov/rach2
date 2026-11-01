/*
 * ath9k.h: the state of the driver of Atheros' AR9003 family, the table
 * of its chips, the calls between its parts, and the layout of the EEPROM.
 *
 * The driver serves one card of the family, whose chips have no firmware,
 * so the driver does the radio itself. A chip is a row of chips.c's table:
 * its PCI ID, its revisions and their initial-value tables, what it has,
 * and the steps it does its own way; the rest of the driver is the
 * family's and names no chip. The AR9565 (QCA9565) of the Lenovo G50-70 is
 * the one row now. The driver follows ath9k of Linux v6.16 (commit
 * 038d61fd642278bab63ee8ef722c50d10ab01e8f), and its parts are ath9k's
 * split: the table (chips.c), the bus (pci.c), the chip's power and reset
 * (hw.c), the EEPROM (eeprom.c), the radio (phy.c), calibration (calib.c) and
 * noise immunity (ani.c), the queues and DMA engines (mac.c), receive
 * (recv.c), transmit (xmit.c), the key cache (key.c), interrupts (intr.c),
 * recovery (recover.c), the antennas (antenna.c), power and the radio
 * switch (power.c) and transmit aggregation (ampdu.c). Each part owns the
 * member of `struct ath9k` named after it and the calls declared under
 * its name below, and reaches the chip only through io.h.
 *
 * Every call below runs with the kernel lock held. "Worker" means the
 * wireless worker of ../../wireless/work.h, which may sleep; "interrupt"
 * means the card's interrupt handler or the timer tick that stands in for
 * it, which may not. Calls return 0 or a negative errno unless they say
 * otherwise.
 *
 * The EEPROM's layout at the end of this file is ath9k's ar9003_eeprom.h
 * with the kernel's integer types, under ath9k's notice:
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
#ifndef _ATH9K_H_
#define _ATH9K_H_

#include "../../../lib/kint.h"
#include "../../../pci.h"
#include "../../wireless/wireless.h"
#include "reg.h"
#include "io.h"

/* ------------------------------------------------------------------ */
/* The EEPROM's layout (ath9k/ar9003_eeprom.h and eeprom.h).           */
/* ------------------------------------------------------------------ */

#define AR9300_EEP_VER            0xD000
#define AR9300_EEP_VER_MINOR_MASK 0xFFF
#define AR9300_EEP_MINOR_VER_1    0x1
#define AR9300_EEP_MINOR_VER      AR9300_EEP_MINOR_VER_1

/* 16-bit offset location start of calibration struct */
#define AR9300_EEP_START_LOC            256
#define AR9300_NUM_5G_CAL_PIERS         8
#define AR9300_NUM_2G_CAL_PIERS         3
#define AR9300_NUM_5G_20_TARGET_POWERS  8
#define AR9300_NUM_5G_40_TARGET_POWERS  8
#define AR9300_NUM_2G_CCK_TARGET_POWERS 2
#define AR9300_NUM_2G_20_TARGET_POWERS  3
#define AR9300_NUM_2G_40_TARGET_POWERS  3
#define AR9300_NUM_CTLS_5G              9
#define AR9300_NUM_CTLS_2G              12
#define AR9300_NUM_BAND_EDGES_5G        8
#define AR9300_NUM_BAND_EDGES_2G        4
#define AR9300_EEPMISC_WOW              0x02
#define AR9300_CUSTOMER_DATA_SIZE       20

#define AR9300_MAX_CHAINS      3
#define AR9300_ANT_16S         25
#define AR9300_FUTURE_MODAL_SZ 6
#define AR_EEPROM_MODAL_SPURS  5

#define AR9300_PAPRD_RATE_MASK 0x01ffffff
#define AR9300_PAPRD_SCALE_1   0x0e000000
#define AR9300_PAPRD_SCALE_1_S 25
#define AR9300_PAPRD_SCALE_2   0x70000000
#define AR9300_PAPRD_SCALE_2_S 28

#define AR9300_EEP_ANTDIV_CONTROL_DEFAULT_VALUE 0xc9

/* Delta from which to start power to pdadc table */
#define AR9300_PWR_TABLE_OFFSET 0

/* Noise power data definitions
 * units are: 4 x dBm - NOISE_PWR_DATA_OFFSET
 * (e.g. -25 = (-25/4 - 90) = -96.25 dBm)
 * range (for 6 signed bits) is (-32 to 31) + offset => -122dBm to -59dBm
 * resolution (2 bits) is 0.25dBm
 */
#define NOISE_PWR_DATA_OFFSET   -90
#define NOISE_PWR_DBM_2_INT(_p) ((((_p) + 3) >> 2) + NOISE_PWR_DATA_OFFSET)
#define N2DBM(_p)               NOISE_PWR_DBM_2_INT(_p)

/* byte addressable */
#define AR9300_EEPROM_SIZE (16 * 1024)

/* The addresses at which the compressed image may begin, searched in
 * ath9k's order: the end of a 1 KiB EEPROM, of a 4 KiB one, of 512 bytes. */
#define AR9300_BASE_ADDR_4K  0xfff
#define AR9300_BASE_ADDR     0x3ff
#define AR9300_BASE_ADDR_512 0x1ff

/* AR5416_EEPMISC_BIG_ENDIAN not set indicates little endian */
#define AR9300_EEPMISC_LITTLE_ENDIAN 0

/* A compressed block's header and checksum, in bytes (ar9003_eeprom.c). */
#define COMP_HDR_LEN   4
#define COMP_CKSUM_LEN 2

/* The conformance test limits' modes (eeprom.h). */
#define CTL_MODE_M 0xf
#define CTL_11B    1
#define CTL_11G    2
#define CTL_2GHT20 5

enum targetPowerHTRates {
    HT_TARGET_RATE_0_8_16,
    HT_TARGET_RATE_1_3_9_11_17_19,
    HT_TARGET_RATE_4,
    HT_TARGET_RATE_5,
    HT_TARGET_RATE_6,
    HT_TARGET_RATE_7,
    HT_TARGET_RATE_12,
    HT_TARGET_RATE_13,
    HT_TARGET_RATE_14,
    HT_TARGET_RATE_15,
    HT_TARGET_RATE_20,
    HT_TARGET_RATE_21,
    HT_TARGET_RATE_22,
    HT_TARGET_RATE_23
};

enum targetPowerLegacyRates {
    LEGACY_TARGET_RATE_6_24,
    LEGACY_TARGET_RATE_36,
    LEGACY_TARGET_RATE_48,
    LEGACY_TARGET_RATE_54
};

enum targetPowerCckRates {
    LEGACY_TARGET_RATE_1L_5L,
    LEGACY_TARGET_RATE_5S,
    LEGACY_TARGET_RATE_11L,
    LEGACY_TARGET_RATE_11S
};

/* The rates a target power is kept for; ar9300RateSize counts them. */
enum ar9300_Rates {
    ALL_TARGET_LEGACY_6_24,
    ALL_TARGET_LEGACY_36,
    ALL_TARGET_LEGACY_48,
    ALL_TARGET_LEGACY_54,
    ALL_TARGET_LEGACY_1L_5L,
    ALL_TARGET_LEGACY_5S,
    ALL_TARGET_LEGACY_11L,
    ALL_TARGET_LEGACY_11S,
    ALL_TARGET_HT20_0_8_16,
    ALL_TARGET_HT20_1_3_9_11_17_19,
    ALL_TARGET_HT20_4,
    ALL_TARGET_HT20_5,
    ALL_TARGET_HT20_6,
    ALL_TARGET_HT20_7,
    ALL_TARGET_HT20_12,
    ALL_TARGET_HT20_13,
    ALL_TARGET_HT20_14,
    ALL_TARGET_HT20_15,
    ALL_TARGET_HT20_20,
    ALL_TARGET_HT20_21,
    ALL_TARGET_HT20_22,
    ALL_TARGET_HT20_23,
    ALL_TARGET_HT40_0_8_16,
    ALL_TARGET_HT40_1_3_9_11_17_19,
    ALL_TARGET_HT40_4,
    ALL_TARGET_HT40_5,
    ALL_TARGET_HT40_6,
    ALL_TARGET_HT40_7,
    ALL_TARGET_HT40_12,
    ALL_TARGET_HT40_13,
    ALL_TARGET_HT40_14,
    ALL_TARGET_HT40_15,
    ALL_TARGET_HT40_20,
    ALL_TARGET_HT40_21,
    ALL_TARGET_HT40_22,
    ALL_TARGET_HT40_23,
    ar9300RateSize
};

/* The ways a block of the image may be compressed; the driver unpacks
 * _CompressNone and _CompressBlock, as ath9k does. */
enum CompressAlgorithm {
    _CompressNone = 0,
    _CompressLzma,
    _CompressPairs,
    _CompressBlock,
    _Compress4,
    _Compress5,
    _Compress6,
    _Compress7
};

/* The image is packed and little-endian, as the card stores it; the 16-
 * and 32-bit members are in the processor's order only on a little-endian
 * processor, which every Rach2 target is. */
struct eepFlags {
    uint8_t opFlags;
    uint8_t eepMisc;
} __attribute__((packed));

struct ar9300_base_eep_hdr {
    uint16_t regDmn[2];
    /* 4 bits tx and 4 bits rx */
    uint8_t txrxMask;
    struct eepFlags opCapFlags;
    uint8_t rfSilent;
    uint8_t blueToothOptions;
    uint8_t deviceCap;
    /* takes lower byte in eeprom location */
    uint8_t deviceType;
    /* offset in dB to be added to beginning
     * of pdadc table in calibration
     */
    int8_t pwrTableOffset;
    uint8_t params_for_tuning_caps[2];
    /*
     * bit0 - enable tx temp comp
     * bit1 - enable tx volt comp
     * bit2 - enable fastClock - default to 1
     * bit3 - enable doubling - default to 1
     * bit4 - enable internal regulator - default to 1
     */
    uint8_t featureEnable;
    /* misc flags: bit0 - turn down drivestrength */
    uint8_t miscConfiguration;
    uint8_t eepromWriteEnableGpio;
    uint8_t wlanDisableGpio;
    uint8_t wlanLedGpio;
    uint8_t rxBandSelectGpio;
    uint8_t txrxgain;
    /* SW controlled internal regulator fields */
    uint32_t swreg;
} __attribute__((packed));

struct ar9300_modal_eep_header {
    /* 4 idle, t1, t2, b (4 bits per setting) */
    uint32_t antCtrlCommon;
    /* 4 ra1l1, ra2l1, ra1l2, ra2l2, ra12 */
    uint32_t antCtrlCommon2;
    /* 6 idle, t, r, rx1, rx12, b (2 bits each) */
    uint16_t antCtrlChain[AR9300_MAX_CHAINS];
    /* 3 xatten1_db for AR9280 (0xa20c/b20c 5:0) */
    uint8_t xatten1DB[AR9300_MAX_CHAINS];
    /* 3  xatten1_margin for merlin (0xa20c/b20c 16:12 */
    uint8_t xatten1Margin[AR9300_MAX_CHAINS];
    int8_t tempSlope;
    int8_t voltSlope;
    /* spur channels in usual fbin coding format */
    uint8_t spurChans[AR_EEPROM_MODAL_SPURS];
    /* 3  Check if the register is per chain */
    int8_t noiseFloorThreshCh[AR9300_MAX_CHAINS];
    uint8_t reserved[11];
    int8_t quick_drop;
    uint8_t xpaBiasLvl;
    uint8_t txFrameToDataStart;
    uint8_t txFrameToPaOn;
    uint8_t txClip;
    int8_t antennaGain;
    uint8_t switchSettling;
    int8_t adcDesiredSize;
    uint8_t txEndToXpaOff;
    uint8_t txEndToRxOn;
    uint8_t txFrameToXpaOn;
    uint8_t thresh62;
    uint32_t papdRateMaskHt20;
    uint32_t papdRateMaskHt40;
    uint16_t switchcomspdt;
    uint8_t xlna_bias_strength;
    uint8_t futureModal[7];
} __attribute__((packed));

struct ar9300_cal_data_per_freq_op_loop {
    int8_t refPower;
    /* pdadc voltage at power measurement */
    uint8_t voltMeas;
    /* pcdac used for power measurement   */
    uint8_t tempMeas;
    /* range is -60 to -127 create a mapping equation 1db resolution */
    int8_t rxNoisefloorCal;
    /*range is same as noisefloor */
    int8_t rxNoisefloorPower;
    /* temp measured when noisefloor cal was performed */
    uint8_t rxTempMeas;
} __attribute__((packed));

struct cal_tgt_pow_legacy {
    uint8_t tPow2x[4];
} __attribute__((packed));

struct cal_tgt_pow_ht {
    uint8_t tPow2x[14];
} __attribute__((packed));

struct cal_ctl_data_2g {
    uint8_t ctlEdges[AR9300_NUM_BAND_EDGES_2G];
} __attribute__((packed));

struct cal_ctl_data_5g {
    uint8_t ctlEdges[AR9300_NUM_BAND_EDGES_5G];
} __attribute__((packed));

#define MAX_BASE_EXTENSION_FUTURE 2

struct ar9300_BaseExtension_1 {
    uint8_t ant_div_control;
    uint8_t future[MAX_BASE_EXTENSION_FUTURE];
    /*
     * misc_enable:
     *
     * BIT 0   - TX Gain Cap enable.
     * BIT 1   - Uncompressed Checksum enable.
     * BIT 2/3 - MinCCApwr enable 2g/5g.
     */
    uint8_t misc_enable;
    int8_t tempslopextension[8];
    int8_t quick_drop_low;
    int8_t quick_drop_high;
} __attribute__((packed));

struct ar9300_BaseExtension_2 {
    int8_t tempSlopeLow;
    int8_t tempSlopeHigh;
    uint8_t xatten1DBLow[AR9300_MAX_CHAINS];
    uint8_t xatten1MarginLow[AR9300_MAX_CHAINS];
    uint8_t xatten1DBHigh[AR9300_MAX_CHAINS];
    uint8_t xatten1MarginHigh[AR9300_MAX_CHAINS];
} __attribute__((packed));

/* The whole image, 1088 bytes, as it is after the compressed blocks are
 * laid over the template the image names. The 5 GHz half, unused on a
 * 2.4 GHz chip, is kept so that a block's offsets land where the card's do. */
struct ar9300_eeprom {
    uint8_t eepromVersion;
    uint8_t templateVersion;
    uint8_t macAddr[6];
    uint8_t custData[AR9300_CUSTOMER_DATA_SIZE];

    struct ar9300_base_eep_hdr baseEepHeader;

    struct ar9300_modal_eep_header modalHeader2G;
    struct ar9300_BaseExtension_1 base_ext1;
    uint8_t calFreqPier2G[AR9300_NUM_2G_CAL_PIERS];
    struct ar9300_cal_data_per_freq_op_loop calPierData2G[AR9300_MAX_CHAINS]
                                                         [AR9300_NUM_2G_CAL_PIERS];
    uint8_t calTarget_freqbin_Cck[AR9300_NUM_2G_CCK_TARGET_POWERS];
    uint8_t calTarget_freqbin_2G[AR9300_NUM_2G_20_TARGET_POWERS];
    uint8_t calTarget_freqbin_2GHT20[AR9300_NUM_2G_20_TARGET_POWERS];
    uint8_t calTarget_freqbin_2GHT40[AR9300_NUM_2G_40_TARGET_POWERS];
    struct cal_tgt_pow_legacy calTargetPowerCck[AR9300_NUM_2G_CCK_TARGET_POWERS];
    struct cal_tgt_pow_legacy calTargetPower2G[AR9300_NUM_2G_20_TARGET_POWERS];
    struct cal_tgt_pow_ht calTargetPower2GHT20[AR9300_NUM_2G_20_TARGET_POWERS];
    struct cal_tgt_pow_ht calTargetPower2GHT40[AR9300_NUM_2G_40_TARGET_POWERS];
    uint8_t ctlIndex_2G[AR9300_NUM_CTLS_2G];
    uint8_t ctl_freqbin_2G[AR9300_NUM_CTLS_2G][AR9300_NUM_BAND_EDGES_2G];
    struct cal_ctl_data_2g ctlPowerData_2G[AR9300_NUM_CTLS_2G];

    struct ar9300_modal_eep_header modalHeader5G;
    struct ar9300_BaseExtension_2 base_ext2;
    uint8_t calFreqPier5G[AR9300_NUM_5G_CAL_PIERS];
    struct ar9300_cal_data_per_freq_op_loop calPierData5G[AR9300_MAX_CHAINS]
                                                         [AR9300_NUM_5G_CAL_PIERS];
    uint8_t calTarget_freqbin_5G[AR9300_NUM_5G_20_TARGET_POWERS];
    uint8_t calTarget_freqbin_5GHT20[AR9300_NUM_5G_20_TARGET_POWERS];
    uint8_t calTarget_freqbin_5GHT40[AR9300_NUM_5G_40_TARGET_POWERS];
    struct cal_tgt_pow_legacy calTargetPower5G[AR9300_NUM_5G_20_TARGET_POWERS];
    struct cal_tgt_pow_ht calTargetPower5GHT20[AR9300_NUM_5G_20_TARGET_POWERS];
    struct cal_tgt_pow_ht calTargetPower5GHT40[AR9300_NUM_5G_40_TARGET_POWERS];
    uint8_t ctlIndex_5G[AR9300_NUM_CTLS_5G];
    uint8_t ctl_freqbin_5G[AR9300_NUM_CTLS_5G][AR9300_NUM_BAND_EDGES_5G];
    struct cal_ctl_data_5g ctlPowerData_5G[AR9300_NUM_CTLS_5G];
} __attribute__((packed));

/* The build fails here if the layout above is not the card's 1088 bytes. */
typedef char ar9300_eeprom_size_is_1088[sizeof(struct ar9300_eeprom) == 1088 ? 1 : -1];

/* ------------------------------------------------------------------ */
/* The driver's constants.                                             */
/* ------------------------------------------------------------------ */

/* Atheros' PCI vendor ID, every chip's (hw.h's ATHEROS_VENDOR_ID). */
#define ATH9K_VENDOR_ID 0x168c

/* ath9k's per-card flags (ath9k.h), which a row gives a card by its
 * subsystem IDs as ath9k's pci.c table does. ATH9K_PCI_1ANT and _2ANT are
 * ath9k's ATH9K_PCI_AR9565_1ANT and _2ANT: a card wired with one antenna,
 * or two for LNA combining. */
#define ATH9K_PCI_CUS198         0x0001
#define ATH9K_PCI_CUS230         0x0002
#define ATH9K_PCI_CUS217         0x0004
#define ATH9K_PCI_CUS252         0x0008
#define ATH9K_PCI_WOW            0x0010
#define ATH9K_PCI_BT_ANT_DIV     0x0020
#define ATH9K_PCI_D3_L1_WAR      0x0040
#define ATH9K_PCI_1ANT           0x0080
#define ATH9K_PCI_2ANT           0x0100
#define ATH9K_PCI_NO_PLL_PWRSAVE 0x0200
#define ATH9K_PCI_KILLER         0x0400
#define ATH9K_PCI_LED_ACT_HI     0x0800

/* What a chip has, in its row's caps. ATH9K_CHIP_JUPITER is a chip of the
 * AR9462's design, which ath9k tests as AR_SREV_9462 || AR_SREV_9565, and
 * ATH9K_CHIP_PCOEM one of ath9k's AR_SREV_9003_PCOEM. */
#define ATH9K_CHIP_2GHZ     0x0001 /* the 2.4 GHz band */
#define ATH9K_CHIP_5GHZ     0x0002 /* the 5 GHz band */
#define ATH9K_CHIP_PCOEM    0x0004 /* a part made for PC makers */
#define ATH9K_CHIP_JUPITER  0x0008 /* of the AR9462's design */
#define ATH9K_CHIP_ANT_COMB 0x0010 /* LNA combining and antenna diversity */
#define ATH9K_CHIP_MCI      0x0020 /* Bluetooth coexistence through MCI */

/* Receive: the two EDMA FIFOs' depths (hw.h's ATH9K_HW_RX_HP_QDEPTH and
 * _LP_QDEPTH), and a buffer's length, ath9k's: the largest MPDU, 3840
 * bytes and the FCS, IV, key ID and ICV, after the 48-byte status, rounded
 * up to 64 bytes. */
#define ATH9K_RX_HP_DEPTH 16
#define ATH9K_RX_LP_DEPTH 128
#define ATH9K_RX_BUF_LEN  3904

/* Transmit: the four access categories' queues, each a ring of frames of
 * which at most ATH9K_TX_FIFO_DEPTH are in the chip's FIFO at once
 * (ath9k's ATH_TXFIFO_DEPTH), the status ring's entries
 * (ATH_TXSTATUS_RING_SIZE) and the attempts a frame gets over its four
 * series (ATH_TXMAXTRY). */
#define ATH9K_TX_QUEUES     4
#define ATH9K_TX_RING       64
#define ATH9K_TX_FIFO_DEPTH 8
#define ATH9K_TXS_RING      512
#define ATH9K_TX_MAX_TRIES  13

/* The noise floor's history (calib.h's NUM_NF_READINGS, the control and
 * extension readings of three chains, and ATH9K_NF_CAL_HIST_MAX). */
#define ATH9K_NF_READINGS 6
#define ATH9K_NF_HISTORY  5

/* The periods of the worker's jobs, in milliseconds (ath9k.h): the short
 * and long calibrations, the noise-immunity check, the hang check, and
 * the full calibration again after 20 minutes. */
#define ATH9K_SHORT_CAL_MS   1000
#define ATH9K_LONG_CAL_MS    30000
#define ATH9K_ANI_MS         1000
#define ATH9K_HANG_MS        1000
#define ATH9K_RESTART_CAL_MS 1200000

/* The `options` bits, set from the kernel command line before the probe. */
#define ATH9K_OPT_NO_ANI   0x01 /* leave noise immunity at its defaults */
#define ATH9K_OPT_NO_ASPM  0x02 /* turn L0s and L1 off on the link */
#define ATH9K_OPT_NO_AMPDU 0x04 /* send single frames only */

/* The column of a five-column ini table for 2.4 GHz HT20 (ath9k's
 * ar9003_hw_process_ini: 5G HT20, 5G HT40, 2G HT40, 2G HT20). */
#define ATH9K_INI_2G_HT20 4

enum ath9k_ini_stage { ATH9K_INI_PRE, ATH9K_INI_CORE, ATH9K_INI_POST, ATH9K_INI_STAGES };

/* The chip's power states (hw.h's enum ath9k_power_mode). */
enum ath9k_power_mode { ATH9K_PM_AWAKE, ATH9K_PM_FULL_SLEEP, ATH9K_PM_NETWORK_SLEEP };

/* The resets of hw.c (hw.h's ATH9K_RESET_*): the RTC's power-on reset, and
 * the warm and cold resets of the MAC and baseband through AR_RTC_RC. */
enum ath9k_reset_type { ATH9K_RESET_POWER_ON, ATH9K_RESET_WARM, ATH9K_RESET_COLD };

/* Why recover.c reset the chip, counted in `struct ath9k_stats`. */
enum ath9k_reset_reason {
    ATH9K_RESET_FATAL,       /* a fatal interrupt: a PCIe error */
    ATH9K_RESET_BB_WATCHDOG, /* the baseband watchdog fired */
    ATH9K_RESET_BB_HANG,     /* the baseband hung (ath9k_hw_check_alive) */
    ATH9K_RESET_MAC_HANG,    /* the MAC's DMA or PCU hung */
    ATH9K_RESET_TX_HANG,     /* a queue held frames past its deadline */
    ATH9K_RESET_RX_STALL,    /* no frame received for too long */
    ATH9K_RESET_CAL,         /* a calibration failed */
    ATH9K_RESET_REASONS
};

/* ------------------------------------------------------------------ */
/* The chips of the family.                                            */
/* ------------------------------------------------------------------ */

/* A card's ATH9K_PCI_* flags by its subsystem IDs. */
struct ath9k_subsys {
    uint16_t vendor;
    uint16_t device;
    unsigned flags;
};

/* A revision a row knows. */
struct ath9k_chip_rev {
    uint32_t mac_rev; /* the AR_SREV_REVISION2 field AR_SREV reads */
    const char *name; /* "1.0", for the probe's line */
};

/* The steps a chip does its own way, which hw.c calls. */
struct ath9k_chip_ops {
    /** Fill ah->hw's initial-value tables for ah->hw.mac_rev, as ath9k's
     * ar9003_hw_init_mode_regs does for the chip. Attach. */
    void (*init_mode_regs)(struct ath9k *ah);
    /** Choose ah->hw's transmit and receive gain tables for the EEPROM's
     * gain modes `tx` and `rx`, the high and low nibbles of
     * baseEepHeader.txrxgain, as ar9003_hw_init_mode_gain_regs does.
     * Attach, after the EEPROM and init_mode_regs. */
    void (*init_gain_tables)(struct ath9k *ah, unsigned tx, unsigned rx);
};

/* The LNA configurations are A-B, LNA2, LNA1 and A+B, numbered 0 to 3. */
struct ath9k_ant_params {
    uint8_t bias[4][4];   /* fast-diversity bias, indexed by main and alternate */
    uint8_t switch_delta; /* minimum RSSI advantage to select LNA2 */
    uint8_t control;      /* nonzero overrides the EEPROM's antenna control byte */
};

/* A chip of the family: a row of chips.c's table. */
struct ath9k_chip {
    const char *name;                  /* "AR9565", for the probe's line */
    uint16_t device;                   /* its PCI device ID */
    uint32_t mac_version;              /* the AR_SREV_VERSION2 field it reads */
    const struct ath9k_chip_rev *revs; /* ending at a NULL name */
    unsigned caps;                     /* ATH9K_CHIP_* */
    uint8_t chainmask;                 /* its chains, one bit each */
    uint8_t num_gpio;                  /* its GPIO lines */
    const struct ath9k_subsys *subsys; /* ending at vendor 0: any other card */
    const struct ath9k_chip_ops *ops;
    const struct ath9k_ant_params *ant; /* NULL without LNA combining */
};

/* ------------------------------------------------------------------ */
/* The state, one member per part.                                     */
/* ------------------------------------------------------------------ */

/* pci.c's: the device, its window and its interrupt. */
struct ath9k_bus {
    pci_device_t dev;
    uintptr_t mmio;         /* the mapped window, 0 before mapping */
    uint16_t subsys_vendor; /* the subsystem IDs, 17aa:NNNN on the G50-70 */
    uint16_t subsys_device;
    unsigned flags; /* ATH9K_PCI_* */
    uint8_t vector; /* the MSI vector, 0 when the tick serves the card */
};

/* hw.c's: the chip's identity, its power, and the tables its row's ops
 * choose for its revision and its EEPROM. */
struct ath9k_hw {
    const struct ath9k_chip *chip;    /* the row, from the PCI ID */
    const struct ath9k_chip_rev *rev; /* the revision AR_SREV read */
    uint32_t mac_version;             /* what AR_SREV read */
    uint32_t mac_rev;
    enum ath9k_power_mode power;
    struct ath9k_ini mac[ATH9K_INI_STAGES];
    struct ath9k_ini bb[ATH9K_INI_STAGES];
    struct ath9k_ini radio[ATH9K_INI_STAGES];
    struct ath9k_ini soc[ATH9K_INI_STAGES];
    struct ath9k_ini rx_gain;
    struct ath9k_ini tx_gain;
    struct ath9k_ini pcie;       /* the PCIe PHY's, around sleep */
    struct ath9k_ini fast_clock; /* the 5 GHz fast clock's */
    struct ath9k_ini japan_2484; /* the CCK filter of channel 14 */
};

/* eeprom.c's: the image and what the driver reads from it once. */
struct ath9k_eeprom {
    struct ar9300_eeprom image; /* unpacked over its template */
    uint16_t regdomain;         /* baseEepHeader.regDmn[0] */
    uint8_t valid;              /* nonzero once image holds a checked image */
};

/* phy.c's: the channel the radio is on and its transmit power. */
struct ath9k_phy {
    struct wlan_channel channel;    /* frequency 0 until the first tune */
    int16_t power_limit;            /* the regulatory cap, in half dB */
    uint8_t target[ar9300RateSize]; /* the power set per rate, in half dB */
};

/* One reading's history of noise-floor results (calib.h's
 * ath9k_nfcal_hist), in dBm. */
struct ath9k_nf_hist {
    int16_t buf[ATH9K_NF_HISTORY];
    uint8_t index;
    uint8_t invalid_count;
    int16_t private_nf; /* the median of buf, which is loaded back */
};

/* calib.c's: what the reset-time and periodic calibrations keep. */
struct ath9k_cal {
    struct ath9k_nf_hist nf[ATH9K_NF_READINGS];
    int16_t noise_floor; /* the channel's noise floor, in dBm */
    uint8_t nf_pending;  /* a noise-floor measurement is running */
    uint8_t iq_valid;    /* the IQ correction below is in use */
    int32_t iq_coeff[2]; /* the transmit IQ correction last applied */
    uint32_t clcal[8];   /* the carrier-leak table, one chain */
    uint8_t temp;        /* the temperature at the last compensation */
    uint64_t last_short; /* worker ticks of the last calibrations */
    uint64_t last_long;
    uint64_t last_full;
    struct wlan_work work; /* the periodic calibration and ANI */
};

/* ani.c's: adaptive noise immunity (ani.h's ar5416AniState). */
struct ath9k_ani {
    uint8_t ofdm_level; /* the OFDM and CCK immunity levels, 0 to 8 */
    uint8_t cck_level;
    uint8_t mrc_cck;
    uint8_t spur_level;
    uint8_t firstep_level;
    uint8_t ofdm_weak_signal;
    uint32_t listen_time; /* milliseconds counted since the last check */
    uint32_t ofdm_errors; /* phy errors counted since the last check */
    uint32_t cck_errors;
};

/* One receive FIFO's buffers, in the order pushed to the chip. */
struct ath9k_rx_fifo {
    uint8_t *buf[ATH9K_RX_LP_DEPTH]; /* ATH9K_RX_BUF_LEN bytes each */
    uint32_t bus[ATH9K_RX_LP_DEPTH]; /* their bus addresses */
    unsigned depth;                  /* ATH9K_RX_HP_DEPTH or _LP_DEPTH */
    unsigned head;                   /* the oldest buffer the chip holds */
    unsigned count;                  /* buffers the chip holds */
};

/* recv.c's: the high- and low-priority FIFOs (ath9k's ath_rx_edma). */
struct ath9k_rx {
    struct ath9k_rx_fifo fifo[2]; /* [0] high priority, [1] low */
    unsigned filters;             /* the WLAN_FILTER_* in force */
    uint8_t running;
};

/* A frame given to a queue, from its acceptance to its status. */
struct ath9k_tx_slot {
    struct ar9003_txc *desc; /* its descriptor */
    uint32_t desc_bus;
    uint8_t *buf; /* a copy of the frame, with room for IV and MIC */
    uint32_t buf_bus;
    unsigned len;
    struct wlan_tx_info info;
};

/* One hardware queue, AR_NUM_QCU's numbering, for one access category. */
struct ath9k_txq {
    struct ath9k_tx_slot slot[ATH9K_TX_RING];
    unsigned qnum;
    unsigned head;   /* the oldest frame not yet completed */
    unsigned count;  /* frames accepted and not completed */
    unsigned queued; /* of those, frames pushed to the chip's FIFO */
    uint64_t oldest; /* worker tick at which the head was pushed */
};

/* xmit.c's: the queues and the status ring. */
struct ath9k_tx {
    struct ath9k_txq q[ATH9K_TX_QUEUES]; /* indexed by enum wlan_ac */
    struct ar9003_txs *status;           /* ATH9K_TXS_RING entries */
    uint32_t status_bus;
    unsigned status_index; /* the next entry the chip will write */
};

/* mac.c's: the interrupt mask and the PCU's settings. */
struct ath9k_mac {
    uint32_t imask; /* AR_IMR bits enabled */
    uint8_t bssid[6];
    uint16_t aid;
    uint8_t slot_time; /* microseconds */
};

/* A key the cache holds, kept to write back after a reset. */
struct ath9k_key {
    uint8_t used;
    uint8_t type; /* AR_KEYTABLE_TYPE_* */
    uint8_t peer[6];
    uint8_t key[16];
    uint8_t mic[16]; /* TKIP's transmit and receive MIC keys */
};

/* key.c's: the cache's 128 entries (AR_KEY_CACHE_SIZE). */
struct ath9k_keys {
    struct ath9k_key entry[AR_KEY_CACHE_SIZE];
};

/* recover.c's: what the hang checks compare between runs. */
struct ath9k_recover {
    uint64_t last_rx;      /* worker tick of the last frame received */
    uint32_t rx_frames;    /* frames received at the last check */
    uint32_t bb_state;     /* the baseband's state at the last check */
    uint8_t bb_same;       /* checks in a row that found it unchanged */
    uint8_t resetting;     /* a reset is running; interrupts are ignored */
    struct wlan_work work; /* the periodic hang check */
};

/* antenna.c's: LNA combining and diversity (ath9k.h's ath_ant_comb). */
struct ath9k_ant {
    uint8_t diversity; /* diversity is on */
    uint8_t main_conf; /* the LNA configurations in use */
    uint8_t alt_conf;
    uint16_t count;
    uint16_t total;
    int main_rssi_sum; /* sums and counts since the last decision */
    int alt_rssi_sum;
    int main_count;
    int alt_count;
    uint8_t combining; /* the EEPROM permits LNA combining */
    uint8_t scanning;
    uint8_t scan_step; /* alternate under test: LNA2/LNA1, A+B, A-B */
    int rssi[4];       /* average RSSI of each configuration during a scan */
};

/* power.c's: the link's ASPM and the radio switch. */
struct ath9k_power_state {
    uint8_t aspm;        /* the link's ASPM control as the firmware set it */
    uint8_t rfkill_gpio; /* the EEPROM's switch line, when rfkill_enabled */
    uint8_t rfkill_polarity;
    uint8_t rfkill_enabled;
    uint8_t hw_blocked; /* the switch's last state */
    uint8_t suspended;
    uint8_t resume;          /* the station wanted to run before suspend */
    uint8_t rfkill_reported; /* a switch sample reached the registered radio */
};

/* One traffic identifier's transmit BlockAck session. */
struct ath9k_tid {
    uint8_t active;
    uint16_t ssn;    /* the window's first sequence number */
    uint16_t window; /* 1 to 64 MPDUs */
};

/* ampdu.c's: the sessions by TID. */
struct ath9k_ampdu {
    struct ath9k_tid tid[8];
};

/* What the driver counts, for the kernel log and the hang checks. */
struct ath9k_stats {
    uint32_t interrupts;
    uint32_t rx_frames;
    uint32_t rx_crc;
    uint32_t rx_phy;
    uint32_t rx_decrypt;
    uint32_t rx_mic;
    uint32_t rx_overrun;
    uint32_t tx_frames;
    uint32_t tx_failed;
    uint32_t tx_retries;
    uint32_t tx_underrun;
    uint32_t beacon_miss;
    uint32_t resets[ATH9K_RESET_REASONS];
};

/* The card. One exists, pci.c's; a host test makes its own. */
struct ath9k {
    const struct ath9k_io *io; /* io.h's indirection */
    void *io_ctx;
    struct wlan_radio radio; /* registered with radio.priv pointing here */
    unsigned options;        /* ATH9K_OPT_* */

    struct ath9k_bus bus;
    struct ath9k_hw hw;
    struct ath9k_eeprom eeprom;
    struct ath9k_phy phy;
    struct ath9k_cal cal;
    struct ath9k_ani ani;
    struct ath9k_rx rx;
    struct ath9k_tx tx;
    struct ath9k_mac mac;
    struct ath9k_keys keys;
    struct ath9k_recover recover;
    struct ath9k_ant ant;
    struct ath9k_power_state power;
    struct ath9k_ampdu ampdu;
    struct ath9k_stats stats;
};

/* ------------------------------------------------------------------ */
/* The calls between the parts.                                        */
/* ------------------------------------------------------------------ */

/* chips.c */

/** The family's chips, ending at NULL. */
extern const struct ath9k_chip *const ath9k_chips[];

/** The row of PCI device `device` under ATH9K_VENDOR_ID, or NULL. */
const struct ath9k_chip *ath9k_chip_find(uint16_t device);

/** The revision `mac_rev` of `chip`, or NULL for one its row does not
 * know. */
const struct ath9k_chip_rev *ath9k_chip_rev(const struct ath9k_chip *chip, uint32_t mac_rev);

/** The ATH9K_PCI_* flags `chip`'s row gives a card with these subsystem
 * IDs: those of the first of its subsys entries that names them, or those
 * of its last entry. */
unsigned ath9k_chip_flags(const struct ath9k_chip *chip, uint16_t subsys_vendor,
                          uint16_t subsys_device);

/* pci.c */

/** The kernel's register access: MMIO on bus.mmio and k_udelay. Its
 * context is the `struct ath9k`. */
extern const struct ath9k_io ath9k_mmio_io;

/** The radio operations of radio.h, which call into the parts below. */
extern const struct wlan_radio_ops ath9k_radio_ops;

/**
 * Find a card of a chip of the table, map its window, bring the chip up and
 * register its radio. Boot, process context. Returns 0, -ENODEV when no
 * card is present, or the errno of the step that failed, which it logs.
 */
int ath9k_probe(void);

/* hw.c */

/**
 * Power the chip on, reset it, read its revision, have hw.chip's ops choose
 * its tables, read the EEPROM and fill the radio's capabilities, as
 * ath9k_hw_init does. Process context. Returns 0, -ENODEV for a chip that
 * reads all ones or reads a version or revision its row does not know, or
 * -EIO.
 */
int ath9k_hw_attach(struct ath9k *ah);

/**
 * Reset the chip onto `chan`, as ath9k_hw_reset does: stop DMA, reset,
 * write the tables, tune, calibrate and set up the queues and the PCU.
 * Interrupts and receive stay off; the caller starts them. Worker.
 */
int ath9k_hw_reset(struct ath9k *ah, const struct wlan_channel *chan);

/** Reset the chip by `type` through AR_RTC_RESET or AR_RTC_RC. Worker or
 * attach. */
int ath9k_hw_set_reset(struct ath9k *ah, enum ath9k_reset_type type);

/** Bring the chip to `mode`, waking it from sleep through
 * AR_RTC_FORCE_WAKE. */
int ath9k_hw_set_power(struct ath9k *ah, enum ath9k_power_mode mode);

/** Stop the chip and leave it in full sleep, as ath9k_hw_disable does. */
void ath9k_hw_disable(struct ath9k *ah);

/* eeprom.c */

/**
 * Read the EEPROM or, failing that, the OTP, unpack the compressed image
 * over its template and check it; fill eeprom, the radio's MAC address and
 * its country. Attach. Returns 0 or -EIO, which leaves eeprom.valid 0.
 */
int ath9k_eeprom_init(struct ath9k *ah);

/** Read the 16-bit EEPROM word at `offset` into *word. */
int ath9k_eeprom_read_word(struct ath9k *ah, unsigned offset, uint16_t *word);

/** Fill `target` with the EEPROM's target power for each ar9300_Rates
 * entry at `freq` MHz, in half dB, interpolated between its piers. */
void ath9k_eeprom_target_power(const struct ath9k *ah, unsigned freq,
                               uint8_t target[ar9300RateSize]);

/** The conformance test limit at `freq` MHz for CTL mode `mode`
 * (CTL_11B, CTL_11G or CTL_2GHT20), in half dB. */
unsigned ath9k_eeprom_ctl_limit(const struct ath9k *ah, unsigned freq, unsigned mode);

/** Write the board's settings for `chan` (bias, drive strength, regulator,
 * tuning caps, quick drop), as ath9k_hw_ar9300_set_board_values does,
 * leaving the antenna's to antenna.c. Worker, within a reset. */
void ath9k_eeprom_set_board_values(struct ath9k *ah, const struct wlan_channel *chan);

/* phy.c */

/** Write the mode, band and gain tables for `chan`, as
 * ar9003_hw_process_ini does. Worker, within a reset. */
void ath9k_phy_process_ini(struct ath9k *ah, const struct wlan_channel *chan);

/** Program the synthesizer for `chan`, HT20, as ar9003_hw_set_channel
 * does, and record it in phy.channel. Worker. */
int ath9k_phy_set_channel(struct ath9k *ah, const struct wlan_channel *chan);

/** Set the per-rate transmit power for `chan` from the EEPROM, capped at
 * `limit` half dB and by the conformance limits. Worker. */
void ath9k_phy_set_txpower(struct ath9k *ah, const struct wlan_channel *chan, int limit);

/* calib.c */

/** Run the reset-time calibrations on `chan`, as ar9003_hw_init_cal does.
 * Worker, within a reset. Returns 0 or -EIO, which asks for a reset. */
int ath9k_cal_init(struct ath9k *ah, const struct wlan_channel *chan);

/** Clear the noise-floor history to the EEPROM's nominal values. */
void ath9k_nf_init(struct ath9k *ah);

/** Start a noise-floor measurement. */
void ath9k_nf_start(struct ath9k *ah);

/** Read a finished measurement into the history and load the median back.
 * Returns 1 when one was read, 0 while it runs, or -EIO when the chip
 * refused the load, which asks for a reset. */
int ath9k_nf_update(struct ath9k *ah);

/** Start the periodic calibration's work; stop it. Worker. */
void ath9k_cal_start(struct ath9k *ah);
void ath9k_cal_stop(struct ath9k *ah);

/* ani.c */

/** Set noise immunity to the defaults; again at each channel change. */
void ath9k_ani_reset(struct ath9k *ah);

/** Read the MIB's phy error counts and raise or lower immunity. Worker,
 * from the calibration's work. Does nothing under ATH9K_OPT_NO_ANI. */
void ath9k_ani_monitor(struct ath9k *ah);

/* mac.c */

/** Set up the QCU and DCU of each data queue, and the status ring. Worker,
 * within a reset. */
void ath9k_mac_init_queues(struct ath9k *ah);

/** Push the descriptor chain at `bus` to queue `qnum`'s FIFO and start it.
 * Interrupt or worker. */
void ath9k_mac_tx_push(struct ath9k *ah, unsigned qnum, uint32_t bus);

/** Stop queue `qnum`, waiting for its frames to leave. -ETIMEDOUT on a
 * queue that does not stop. */
int ath9k_mac_stop_queue(struct ath9k *ah, unsigned qnum);

/** Abort every queue's DMA, as ath9k_hw_abort_tx_dma does. */
int ath9k_mac_abort_tx(struct ath9k *ah);

/** Push the buffer at `bus` to receive FIFO `hp` (1 high, 0 low). */
void ath9k_mac_rx_push(struct ath9k *ah, int hp, uint32_t bus);

/** Start the PCU's receive; stop it and the receive DMA, as
 * ath9k_hw_stopdmarecv does. -ETIMEDOUT when the DMA does not stop. */
void ath9k_mac_rx_start(struct ath9k *ah);
int ath9k_mac_rx_stop(struct ath9k *ah);

/** Set the receive filter from WLAN_FILTER_* bits and the station's state. */
void ath9k_mac_set_filter(struct ath9k *ah, unsigned filters);

/** Set the MAC address, and the BSSID and association ID (all zero when
 * not associated). */
void ath9k_mac_set_address(struct ath9k *ah);
void ath9k_mac_set_bssid(struct ath9k *ah, const uint8_t bssid[6], unsigned aid);

/** Enable the interrupts of mac.imask; disable all, waiting for none to be
 * in flight. Interrupt or worker. */
void ath9k_mac_intr_enable(struct ath9k *ah);
void ath9k_mac_intr_disable(struct ath9k *ah);

/* recv.c */

/** Allocate the receive buffers. Attach. -ENOMEM. */
int ath9k_rx_init(struct ath9k *ah);

/** Give every buffer to the chip and start receiving; stop, taking the
 * buffers back. Worker. */
int ath9k_rx_start(struct ath9k *ah);
int ath9k_rx_stop(struct ath9k *ah);

/** Hand each completed buffer of FIFO `hp` to wlan_radio_rx and give it
 * back to the chip. Interrupt. Returns the frames delivered. */
unsigned ath9k_rx_service(struct ath9k *ah, int hp);

/* xmit.c */

/** Allocate the descriptors, frame buffers and status ring. Attach. */
int ath9k_tx_init(struct ath9k *ah);

/** The radio's tx operation: copy the frame to a slot of info->ac's queue
 * and push it. -ENOBUFS when the queue is full. Worker. */
int ath9k_tx(struct ath9k *ah, const uint8_t *frame, unsigned len, const struct wlan_tx_info *info);

/** Read the status ring and report each finished frame with
 * wlan_radio_tx_status. Interrupt. Returns the frames finished. */
unsigned ath9k_tx_service(struct ath9k *ah);

/** Report every accepted frame as failed and empty the queues, after the
 * DMA is stopped. Worker. */
void ath9k_tx_drain(struct ath9k *ah);

/* key.c */

/** Install `key` for `peer` in a free entry and set *slot, or
 * -EOPNOTSUPP for a cipher the cache does not do. Worker. */
int ath9k_key_set(struct ath9k *ah, const uint8_t peer[6], const struct wlan_key *key, int *slot);

/** Clear entry `slot`. Worker. */
void ath9k_key_clear(struct ath9k *ah, int slot);

/** Clear the whole cache, then write back each entry in use. Worker,
 * after a reset. */
void ath9k_key_restore(struct ath9k *ah);

/* intr.c */

/** Take a vector and enable MSI, or fall back to the tick. Attach. */
int ath9k_intr_attach(struct ath9k *ah);

/** Serve the chip's interrupt causes: receive, transmit, beacon miss, and
 * the fatal causes, which ask recover.c for a reset. Interrupt. */
void ath9k_intr(struct ath9k *ah);

/** The tick's service of the rings when no message arrives. Interrupt. */
void ath9k_tick(struct ath9k *ah);

/* recover.c */

/** Reset the chip on its channel and bring the station's state back: keys,
 * filter, BSSID, queues and receive. Worker. Counts `why`. */
int ath9k_reset(struct ath9k *ah, enum ath9k_reset_reason why);

/** Ask the worker for a reset, from interrupt context. */
void ath9k_reset_schedule(struct ath9k *ah, enum ath9k_reset_reason why);

/** Start the periodic hang check; stop it. Worker. */
void ath9k_recover_start(struct ath9k *ah);
void ath9k_recover_stop(struct ath9k *ah);

/* antenna.c */

/** Choose the antenna configuration from bus.flags and the EEPROM. Attach. */
void ath9k_ant_init(struct ath9k *ah);

/** Write the antenna switch tables and the LNA configuration, Bluetooth
 * unused. Worker, within a reset. */
void ath9k_ant_set(struct ath9k *ah);

/** Count a received frame's signal on the main and alternate LNAs, and
 * change the configuration when diversity decides to. Interrupt. */
void ath9k_ant_rx(struct ath9k *ah, const struct ar9003_rxs *rxs);

/* power.c */

/** Read the link's ASPM, turn it off under ATH9K_OPT_NO_ASPM, and set up
 * the radio switch from the EEPROM. Attach. */
void ath9k_power_init(struct ath9k *ah);

/** Read the radio switch and report a change with wlan_radio_rfkill. */
void ath9k_rfkill_poll(struct ath9k *ah);

/** Stop the station before suspend to idle; start it after if it was wanted.
 * Process context, no net_lock. Repeated calls are safe. A blocked station
 * stays stopped until rfkill permits it. A failed resume may be retried. */
void ath9k_suspend(struct ath9k *ah);
int ath9k_resume(struct ath9k *ah);

/** Suspend or resume the one attached card, if any. Process context under
 * the kernel lock, no net_lock; used by suspend_to_idle. */
void ath9k_suspend_all(void);
int ath9k_resume_all(void);

/* ampdu.c */

/** The radio's ampdu_action operation. -EOPNOTSUPP for the receive actions,
 * and for every action under ATH9K_OPT_NO_AMPDU. Worker. */
int ath9k_ampdu_action(struct ath9k *ah, enum wlan_ampdu_action action, const uint8_t peer[6],
                       unsigned tid, unsigned ssn, unsigned window);

/** Take a finished aggregate's status and BlockAck bitmap. Interrupt. */
void ath9k_ampdu_status(struct ath9k *ah, const struct ar9003_txs *ts);

#endif
