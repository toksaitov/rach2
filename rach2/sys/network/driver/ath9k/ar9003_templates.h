/*
 * ar9003_templates.h: ath9k's five board templates of the AR9300 EEPROM,
 * over one of which eeprom.c lays a card's compressed blocks. A block
 * names its template by templateVersion: 2 the reference design's, 3 the
 * x112's, 4 the h116's (the Lenovo G50-70's card), 5 the h112's and 6 the
 * x113's. They are ath9k's ar9300_default, ar9300_x112, ar9300_h116,
 * ar9300_h112 and ar9300_x113 of ar9003_eeprom.c, value for value, written
 * with positional initializers in the order of `struct ar9300_eeprom`.
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

#ifndef _ATH9K_AR9003_TEMPLATES_H_
#define _ATH9K_AR9003_TEMPLATES_H_

#include "ath9k.h"

/* A frequency's bin (eeprom.h): MHz above 2300 at 2.4 GHz, and fifths
 * above 4800 at 5 GHz. */
#define FREQ2FBIN(x, y) (uint8_t)((y) ? ((x) - 2300) : (((x) - 4800) / 5))

/* A conformance limit's band edge (ar9003_eeprom.c): its power in half dB
 * and, in the top two bits, whether it holds up to the next edge. */
#define CTL(_tpower, _flag) ((_tpower) | ((_flag) << 6))

static const struct ar9300_eeprom ar9300_default = {
    2,                                    /* eepromVersion */
    2,                                    /* templateVersion */
    {0x00, 0x02, 0x03, 0x04, 0x05, 0x06}, /* macAddr */
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
     0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, /* custData */
    {
        /* baseEepHeader */
        {0x0, 0x1f},  /* regDmn */
        0x77,         /* txrxMask */
        {0x03, 0x00}, /* opCapFlags */
        0,            /* rfSilent */
        0,            /* blueToothOptions */
        0,            /* deviceCap */
        5,            /* deviceType */
        0,            /* pwrTableOffset */
        {0, 0},       /* params_for_tuning_caps */
        0x0c,         /* featureEnable */
        0x00,         /* miscConfiguration */
        3,            /* eepromWriteEnableGpio */
        0,            /* wlanDisableGpio */
        8,            /* wlanLedGpio */
        255,          /* rxBandSelectGpio */
        0x00,         /* txrxgain */
        0x0,          /* swreg */
    },
    {
        /* modalHeader2G */
        0x110,                             /* antCtrlCommon */
        0x22222,                           /* antCtrlCommon2 */
        {0x150, 0x150, 0x150},             /* antCtrlChain */
        {0, 0, 0},                         /* xatten1DB */
        {0, 0, 0},                         /* xatten1Margin */
        36,                                /* tempSlope */
        0,                                 /* voltSlope */
        {0, 0, 0, 0, 0},                   /* spurChans */
        {-1, 0, 0},                        /* noiseFloorThreshCh */
        {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, /* reserved */
        0,                                 /* quick_drop */
        0,                                 /* xpaBiasLvl */
        14,                                /* txFrameToDataStart */
        14,                                /* txFrameToPaOn */
        3,                                 /* txClip */
        0,                                 /* antennaGain */
        44,                                /* switchSettling */
        -30,                               /* adcDesiredSize */
        0,                                 /* txEndToXpaOff */
        2,                                 /* txEndToRxOn */
        14,                                /* txFrameToXpaOn */
        28,                                /* thresh62 */
        0xcf0e0e0,                         /* papdRateMaskHt20 */
        0x6cf0e0e0,                        /* papdRateMaskHt40 */
        0x0,                               /* switchcomspdt */
        0,                                 /* xlna_bias_strength */
        {0, 0, 0, 0, 0, 0, 0},             /* futureModal */
    },
    {
        /* base_ext1 */
        0x00,                     /* ant_div_control */
        {0, 0},                   /* future */
        0x00,                     /* misc_enable */
        {0, 0, 0, 0, 0, 0, 0, 0}, /* tempslopextension */
        0,                        /* quick_drop_low */
        0,                        /* quick_drop_high */
    },
    {FREQ2FBIN(2412, 1), FREQ2FBIN(2437, 1), FREQ2FBIN(2472, 1)}, /* calFreqPier2G */
    {
        /* calPierData2G */
        {{0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0}},
        {{0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0}},
        {{0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0}},
    },
    {FREQ2FBIN(2412, 1), FREQ2FBIN(2484, 1)},                     /* calTarget_freqbin_Cck */
    {FREQ2FBIN(2412, 1), FREQ2FBIN(2437, 1), FREQ2FBIN(2472, 1)}, /* calTarget_freqbin_2G */
    {FREQ2FBIN(2412, 1), FREQ2FBIN(2437, 1), FREQ2FBIN(2472, 1)}, /* calTarget_freqbin_2GHT20 */
    {FREQ2FBIN(2412, 1), FREQ2FBIN(2437, 1), FREQ2FBIN(2472, 1)}, /* calTarget_freqbin_2GHT40 */
    {
        /* calTargetPowerCck */
        {{36, 36, 36, 36}},
        {{36, 36, 36, 36}},
    },
    {
        /* calTargetPower2G */
        {{32, 32, 28, 24}},
        {{32, 32, 28, 24}},
        {{32, 32, 28, 24}},
    },
    {
        /* calTargetPower2GHT20 */
        {{32, 32, 32, 32, 28, 20, 32, 32, 28, 20, 32, 32, 28, 20}},
        {{32, 32, 32, 32, 28, 20, 32, 32, 28, 20, 32, 32, 28, 20}},
        {{32, 32, 32, 32, 28, 20, 32, 32, 28, 20, 32, 32, 28, 20}},
    },
    {
        /* calTargetPower2GHT40 */
        {{32, 32, 32, 32, 28, 20, 32, 32, 28, 20, 32, 32, 28, 20}},
        {{32, 32, 32, 32, 28, 20, 32, 32, 28, 20, 32, 32, 28, 20}},
        {{32, 32, 32, 32, 28, 20, 32, 32, 28, 20, 32, 32, 28, 20}},
    },
    {0x11, 0x12, 0x15, 0x17, 0x41, 0x42, 0x45, 0x47, 0x31, 0x32, 0x35, 0x37}, /* ctlIndex_2G */
    {
        /* ctl_freqbin_2G */
        {FREQ2FBIN(2412, 1), FREQ2FBIN(2417, 1), FREQ2FBIN(2457, 1), FREQ2FBIN(2462, 1)},
        {FREQ2FBIN(2412, 1), FREQ2FBIN(2417, 1), FREQ2FBIN(2462, 1), 0xff},
        {FREQ2FBIN(2412, 1), FREQ2FBIN(2417, 1), FREQ2FBIN(2462, 1), 0xff},
        {FREQ2FBIN(2422, 1), FREQ2FBIN(2427, 1), FREQ2FBIN(2447, 1), FREQ2FBIN(2452, 1)},
        {FREQ2FBIN(2412, 1), FREQ2FBIN(2417, 1), FREQ2FBIN(2472, 1), FREQ2FBIN(2484, 1)},
        {FREQ2FBIN(2412, 1), FREQ2FBIN(2417, 1), FREQ2FBIN(2472, 1), 0x00},
        {FREQ2FBIN(2412, 1), FREQ2FBIN(2417, 1), FREQ2FBIN(2472, 1), 0x00},
        {FREQ2FBIN(2422, 1), FREQ2FBIN(2427, 1), FREQ2FBIN(2447, 1), FREQ2FBIN(2462, 1)},
        {FREQ2FBIN(2412, 1), FREQ2FBIN(2417, 1), FREQ2FBIN(2472, 1), 0x00},
        {FREQ2FBIN(2412, 1), FREQ2FBIN(2417, 1), FREQ2FBIN(2472, 1), 0x00},
        {FREQ2FBIN(2412, 1), FREQ2FBIN(2417, 1), FREQ2FBIN(2472, 1), 0x00},
        {FREQ2FBIN(2422, 1), FREQ2FBIN(2427, 1), FREQ2FBIN(2447, 1), FREQ2FBIN(2462, 1)},
    },
    {
        /* ctlPowerData_2G */
        {{CTL(60, 0), CTL(60, 1), CTL(60, 0), CTL(60, 0)}},
        {{CTL(60, 0), CTL(60, 1), CTL(60, 0), CTL(60, 0)}},
        {{CTL(60, 1), CTL(60, 0), CTL(60, 0), CTL(60, 1)}},
        {{CTL(60, 1), CTL(60, 0), CTL(60, 0), CTL(60, 0)}},
        {{CTL(60, 0), CTL(60, 1), CTL(60, 0), CTL(60, 0)}},
        {{CTL(60, 0), CTL(60, 1), CTL(60, 0), CTL(60, 0)}},
        {{CTL(60, 0), CTL(60, 1), CTL(60, 1), CTL(60, 0)}},
        {{CTL(60, 0), CTL(60, 1), CTL(60, 0), CTL(60, 0)}},
        {{CTL(60, 0), CTL(60, 1), CTL(60, 0), CTL(60, 0)}},
        {{CTL(60, 0), CTL(60, 1), CTL(60, 0), CTL(60, 0)}},
        {{CTL(60, 0), CTL(60, 1), CTL(60, 1), CTL(60, 1)}},
        {{CTL(60, 0), CTL(60, 1), CTL(60, 1), CTL(60, 1)}},
    },
    {
        /* modalHeader5G */
        0x110,                             /* antCtrlCommon */
        0x22222,                           /* antCtrlCommon2 */
        {0x0, 0x0, 0x0},                   /* antCtrlChain */
        {0, 0, 0},                         /* xatten1DB */
        {0, 0, 0},                         /* xatten1Margin */
        68,                                /* tempSlope */
        0,                                 /* voltSlope */
        {0, 0, 0, 0, 0},                   /* spurChans */
        {-1, 0, 0},                        /* noiseFloorThreshCh */
        {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, /* reserved */
        0,                                 /* quick_drop */
        0,                                 /* xpaBiasLvl */
        14,                                /* txFrameToDataStart */
        14,                                /* txFrameToPaOn */
        3,                                 /* txClip */
        0,                                 /* antennaGain */
        45,                                /* switchSettling */
        -30,                               /* adcDesiredSize */
        0,                                 /* txEndToXpaOff */
        2,                                 /* txEndToRxOn */
        14,                                /* txFrameToXpaOn */
        28,                                /* thresh62 */
        0xc80c080,                         /* papdRateMaskHt20 */
        0x80c080,                          /* papdRateMaskHt40 */
        0x0,                               /* switchcomspdt */
        0,                                 /* xlna_bias_strength */
        {0, 0, 0, 0, 0, 0, 0},             /* futureModal */
    },
    {
        /* base_ext2 */
        0,         /* tempSlopeLow */
        0,         /* tempSlopeHigh */
        {0, 0, 0}, /* xatten1DBLow */
        {0, 0, 0}, /* xatten1MarginLow */
        {0, 0, 0}, /* xatten1DBHigh */
        {0, 0, 0}, /* xatten1MarginHigh */
    },
    {FREQ2FBIN(5180, 0), FREQ2FBIN(5220, 0), FREQ2FBIN(5320, 0), FREQ2FBIN(5400, 0),
     FREQ2FBIN(5500, 0), FREQ2FBIN(5600, 0), FREQ2FBIN(5725, 0),
     FREQ2FBIN(5825, 0)}, /* calFreqPier5G */
    {
        /* calPierData5G */
        {{0, 0, 0, 0, 0, 0},
         {0, 0, 0, 0, 0, 0},
         {0, 0, 0, 0, 0, 0},
         {0, 0, 0, 0, 0, 0},
         {0, 0, 0, 0, 0, 0},
         {0, 0, 0, 0, 0, 0},
         {0, 0, 0, 0, 0, 0},
         {0, 0, 0, 0, 0, 0}},
        {{0, 0, 0, 0, 0, 0},
         {0, 0, 0, 0, 0, 0},
         {0, 0, 0, 0, 0, 0},
         {0, 0, 0, 0, 0, 0},
         {0, 0, 0, 0, 0, 0},
         {0, 0, 0, 0, 0, 0},
         {0, 0, 0, 0, 0, 0},
         {0, 0, 0, 0, 0, 0}},
        {{0, 0, 0, 0, 0, 0},
         {0, 0, 0, 0, 0, 0},
         {0, 0, 0, 0, 0, 0},
         {0, 0, 0, 0, 0, 0},
         {0, 0, 0, 0, 0, 0},
         {0, 0, 0, 0, 0, 0},
         {0, 0, 0, 0, 0, 0},
         {0, 0, 0, 0, 0, 0}},
    },
    {FREQ2FBIN(5180, 0), FREQ2FBIN(5220, 0), FREQ2FBIN(5320, 0), FREQ2FBIN(5400, 0),
     FREQ2FBIN(5500, 0), FREQ2FBIN(5600, 0), FREQ2FBIN(5725, 0),
     FREQ2FBIN(5825, 0)}, /* calTarget_freqbin_5G */
    {FREQ2FBIN(5180, 0), FREQ2FBIN(5240, 0), FREQ2FBIN(5320, 0), FREQ2FBIN(5500, 0),
     FREQ2FBIN(5700, 0), FREQ2FBIN(5745, 0), FREQ2FBIN(5725, 0),
     FREQ2FBIN(5825, 0)}, /* calTarget_freqbin_5GHT20 */
    {FREQ2FBIN(5180, 0), FREQ2FBIN(5240, 0), FREQ2FBIN(5320, 0), FREQ2FBIN(5500, 0),
     FREQ2FBIN(5700, 0), FREQ2FBIN(5745, 0), FREQ2FBIN(5725, 0),
     FREQ2FBIN(5825, 0)}, /* calTarget_freqbin_5GHT40 */
    {
        /* calTargetPower5G */
        {{20, 20, 20, 10}},
        {{20, 20, 20, 10}},
        {{20, 20, 20, 10}},
        {{20, 20, 20, 10}},
        {{20, 20, 20, 10}},
        {{20, 20, 20, 10}},
        {{20, 20, 20, 10}},
        {{20, 20, 20, 10}},
    },
    {
        /* calTargetPower5GHT20 */
        {{20, 20, 10, 10, 0, 0, 10, 10, 0, 0, 10, 10, 0, 0}},
        {{20, 20, 10, 10, 0, 0, 10, 10, 0, 0, 10, 10, 0, 0}},
        {{20, 20, 10, 10, 0, 0, 10, 10, 0, 0, 10, 10, 0, 0}},
        {{20, 20, 10, 10, 0, 0, 10, 10, 0, 0, 10, 10, 0, 0}},
        {{20, 20, 10, 10, 0, 0, 10, 10, 0, 0, 10, 10, 0, 0}},
        {{20, 20, 10, 10, 0, 0, 10, 10, 0, 0, 10, 10, 0, 0}},
        {{20, 20, 10, 10, 0, 0, 10, 10, 0, 0, 10, 10, 0, 0}},
        {{20, 20, 10, 10, 0, 0, 10, 10, 0, 0, 10, 10, 0, 0}},
    },
    {
        /* calTargetPower5GHT40 */
        {{20, 20, 10, 10, 0, 0, 10, 10, 0, 0, 10, 10, 0, 0}},
        {{20, 20, 10, 10, 0, 0, 10, 10, 0, 0, 10, 10, 0, 0}},
        {{20, 20, 10, 10, 0, 0, 10, 10, 0, 0, 10, 10, 0, 0}},
        {{20, 20, 10, 10, 0, 0, 10, 10, 0, 0, 10, 10, 0, 0}},
        {{20, 20, 10, 10, 0, 0, 10, 10, 0, 0, 10, 10, 0, 0}},
        {{20, 20, 10, 10, 0, 0, 10, 10, 0, 0, 10, 10, 0, 0}},
        {{20, 20, 10, 10, 0, 0, 10, 10, 0, 0, 10, 10, 0, 0}},
        {{20, 20, 10, 10, 0, 0, 10, 10, 0, 0, 10, 10, 0, 0}},
    },
    {0x10, 0x16, 0x18, 0x40, 0x46, 0x48, 0x30, 0x36, 0x38}, /* ctlIndex_5G */
    {
        /* ctl_freqbin_5G */
        {FREQ2FBIN(5180, 0), FREQ2FBIN(5260, 0), FREQ2FBIN(5280, 0), FREQ2FBIN(5500, 0),
         FREQ2FBIN(5600, 0), FREQ2FBIN(5700, 0), FREQ2FBIN(5745, 0), FREQ2FBIN(5825, 0)},
        {FREQ2FBIN(5180, 0), FREQ2FBIN(5260, 0), FREQ2FBIN(5280, 0), FREQ2FBIN(5500, 0),
         FREQ2FBIN(5520, 0), FREQ2FBIN(5700, 0), FREQ2FBIN(5745, 0), FREQ2FBIN(5825, 0)},
        {FREQ2FBIN(5190, 0), FREQ2FBIN(5230, 0), FREQ2FBIN(5270, 0), FREQ2FBIN(5310, 0),
         FREQ2FBIN(5510, 0), FREQ2FBIN(5550, 0), FREQ2FBIN(5670, 0), FREQ2FBIN(5755, 0)},
        {FREQ2FBIN(5180, 0), FREQ2FBIN(5200, 0), FREQ2FBIN(5260, 0), FREQ2FBIN(5320, 0),
         FREQ2FBIN(5500, 0), FREQ2FBIN(5700, 0), 0xff, 0xff},
        {FREQ2FBIN(5180, 0), FREQ2FBIN(5260, 0), FREQ2FBIN(5500, 0), FREQ2FBIN(5700, 0), 0xff, 0xff,
         0xff, 0xff},
        {FREQ2FBIN(5190, 0), FREQ2FBIN(5270, 0), FREQ2FBIN(5310, 0), FREQ2FBIN(5510, 0),
         FREQ2FBIN(5590, 0), FREQ2FBIN(5670, 0), 0xff, 0xff},
        {FREQ2FBIN(5180, 0), FREQ2FBIN(5200, 0), FREQ2FBIN(5220, 0), FREQ2FBIN(5260, 0),
         FREQ2FBIN(5500, 0), FREQ2FBIN(5600, 0), FREQ2FBIN(5700, 0), FREQ2FBIN(5745, 0)},
        {FREQ2FBIN(5180, 0), FREQ2FBIN(5260, 0), FREQ2FBIN(5320, 0), FREQ2FBIN(5500, 0),
         FREQ2FBIN(5560, 0), FREQ2FBIN(5700, 0), FREQ2FBIN(5745, 0), FREQ2FBIN(5825, 0)},
        {FREQ2FBIN(5190, 0), FREQ2FBIN(5230, 0), FREQ2FBIN(5270, 0), FREQ2FBIN(5510, 0),
         FREQ2FBIN(5550, 0), FREQ2FBIN(5670, 0), FREQ2FBIN(5755, 0), FREQ2FBIN(5795, 0)},
    },
    {
        /* ctlPowerData_5G */
        {{CTL(60, 1), CTL(60, 1), CTL(60, 1), CTL(60, 1), CTL(60, 1), CTL(60, 1), CTL(60, 1),
          CTL(60, 0)}},
        {{CTL(60, 1), CTL(60, 1), CTL(60, 1), CTL(60, 1), CTL(60, 1), CTL(60, 1), CTL(60, 1),
          CTL(60, 0)}},
        {{CTL(60, 0), CTL(60, 1), CTL(60, 0), CTL(60, 1), CTL(60, 1), CTL(60, 1), CTL(60, 1),
          CTL(60, 1)}},
        {{CTL(60, 0), CTL(60, 1), CTL(60, 1), CTL(60, 0), CTL(60, 1), CTL(60, 0), CTL(60, 0),
          CTL(60, 0)}},
        {{CTL(60, 1), CTL(60, 1), CTL(60, 1), CTL(60, 0), CTL(60, 0), CTL(60, 0), CTL(60, 0),
          CTL(60, 0)}},
        {{CTL(60, 1), CTL(60, 1), CTL(60, 1), CTL(60, 1), CTL(60, 1), CTL(60, 0), CTL(60, 0),
          CTL(60, 0)}},
        {{CTL(60, 1), CTL(60, 1), CTL(60, 1), CTL(60, 1), CTL(60, 1), CTL(60, 1), CTL(60, 1),
          CTL(60, 1)}},
        {{CTL(60, 1), CTL(60, 1), CTL(60, 0), CTL(60, 1), CTL(60, 1), CTL(60, 1), CTL(60, 1),
          CTL(60, 0)}},
        {{CTL(60, 1), CTL(60, 0), CTL(60, 1), CTL(60, 1), CTL(60, 1), CTL(60, 1), CTL(60, 0),
          CTL(60, 1)}},
    },
};

static const struct ar9300_eeprom ar9300_x112 = {
    2,                                    /* eepromVersion */
    5,                                    /* templateVersion */
    {0x00, 0x03, 0x7f, 0x00, 0x00, 0x00}, /* macAddr */
    {0x78, 0x31, 0x31, 0x32, 0x2d, 0x30, 0x34, 0x31, 0x2d, 0x66,
     0x30, 0x30, 0x30, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, /* custData */
    {
        /* baseEepHeader */
        {0x0, 0x1f},  /* regDmn */
        0x77,         /* txrxMask */
        {0x03, 0x00}, /* opCapFlags */
        0,            /* rfSilent */
        0,            /* blueToothOptions */
        0,            /* deviceCap */
        5,            /* deviceType */
        0,            /* pwrTableOffset */
        {0, 0},       /* params_for_tuning_caps */
        0x0d,         /* featureEnable */
        0x00,         /* miscConfiguration */
        6,            /* eepromWriteEnableGpio */
        0,            /* wlanDisableGpio */
        8,            /* wlanLedGpio */
        255,          /* rxBandSelectGpio */
        0x00,         /* txrxgain */
        0x0,          /* swreg */
    },
    {
        /* modalHeader2G */
        0x110,                             /* antCtrlCommon */
        0x22222,                           /* antCtrlCommon2 */
        {0x10, 0x10, 0x10},                /* antCtrlChain */
        {27, 27, 27},                      /* xatten1DB */
        {21, 21, 21},                      /* xatten1Margin */
        50,                                /* tempSlope */
        0,                                 /* voltSlope */
        {164, 0, 0, 0, 0},                 /* spurChans */
        {-1, 0, 0},                        /* noiseFloorThreshCh */
        {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, /* reserved */
        0,                                 /* quick_drop */
        0,                                 /* xpaBiasLvl */
        14,                                /* txFrameToDataStart */
        14,                                /* txFrameToPaOn */
        3,                                 /* txClip */
        0,                                 /* antennaGain */
        44,                                /* switchSettling */
        -30,                               /* adcDesiredSize */
        0,                                 /* txEndToXpaOff */
        2,                                 /* txEndToRxOn */
        14,                                /* txFrameToXpaOn */
        28,                                /* thresh62 */
        0xc80c080,                         /* papdRateMaskHt20 */
        0x80c080,                          /* papdRateMaskHt40 */
        0x0,                               /* switchcomspdt */
        0,                                 /* xlna_bias_strength */
        {0, 0, 0, 0, 0, 0, 0},             /* futureModal */
    },
    {
        /* base_ext1 */
        0x00,                     /* ant_div_control */
        {0, 0},                   /* future */
        0x00,                     /* misc_enable */
        {0, 0, 0, 0, 0, 0, 0, 0}, /* tempslopextension */
        0,                        /* quick_drop_low */
        0,                        /* quick_drop_high */
    },
    {FREQ2FBIN(2412, 1), FREQ2FBIN(2437, 1), FREQ2FBIN(2472, 1)}, /* calFreqPier2G */
    {
        /* calPierData2G */
        {{0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0}},
        {{0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0}},
        {{0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0}},
    },
    {FREQ2FBIN(2412, 1), FREQ2FBIN(2472, 1)},                     /* calTarget_freqbin_Cck */
    {FREQ2FBIN(2412, 1), FREQ2FBIN(2437, 1), FREQ2FBIN(2472, 1)}, /* calTarget_freqbin_2G */
    {FREQ2FBIN(2412, 1), FREQ2FBIN(2437, 1), FREQ2FBIN(2472, 1)}, /* calTarget_freqbin_2GHT20 */
    {FREQ2FBIN(2412, 1), FREQ2FBIN(2437, 1), FREQ2FBIN(2472, 1)}, /* calTarget_freqbin_2GHT40 */
    {
        /* calTargetPowerCck */
        {{38, 38, 38, 38}},
        {{38, 38, 38, 38}},
    },
    {
        /* calTargetPower2G */
        {{38, 38, 36, 34}},
        {{38, 38, 36, 34}},
        {{38, 38, 34, 32}},
    },
    {
        /* calTargetPower2GHT20 */
        {{36, 36, 36, 36, 36, 34, 34, 32, 30, 28, 28, 28, 28, 26}},
        {{36, 36, 36, 36, 36, 34, 36, 34, 32, 30, 30, 30, 28, 26}},
        {{36, 36, 36, 36, 36, 34, 34, 32, 30, 28, 28, 28, 28, 26}},
    },
    {
        /* calTargetPower2GHT40 */
        {{36, 36, 36, 36, 34, 32, 32, 30, 28, 26, 26, 26, 26, 24}},
        {{36, 36, 36, 36, 34, 32, 34, 32, 30, 28, 28, 28, 28, 24}},
        {{36, 36, 36, 36, 34, 32, 32, 30, 28, 26, 26, 26, 26, 24}},
    },
    {0x11, 0x12, 0x15, 0x17, 0x41, 0x42, 0x45, 0x47, 0x31, 0x32, 0x35, 0x37}, /* ctlIndex_2G */
    {
        /* ctl_freqbin_2G */
        {FREQ2FBIN(2412, 1), FREQ2FBIN(2417, 1), FREQ2FBIN(2457, 1), FREQ2FBIN(2462, 1)},
        {FREQ2FBIN(2412, 1), FREQ2FBIN(2417, 1), FREQ2FBIN(2462, 1), 0xff},
        {FREQ2FBIN(2412, 1), FREQ2FBIN(2417, 1), FREQ2FBIN(2462, 1), 0xff},
        {FREQ2FBIN(2422, 1), FREQ2FBIN(2427, 1), FREQ2FBIN(2447, 1), FREQ2FBIN(2452, 1)},
        {FREQ2FBIN(2412, 1), FREQ2FBIN(2417, 1), FREQ2FBIN(2472, 1), FREQ2FBIN(2484, 1)},
        {FREQ2FBIN(2412, 1), FREQ2FBIN(2417, 1), FREQ2FBIN(2472, 1), 0x00},
        {FREQ2FBIN(2412, 1), FREQ2FBIN(2417, 1), FREQ2FBIN(2472, 1), 0x00},
        {FREQ2FBIN(2422, 1), FREQ2FBIN(2427, 1), FREQ2FBIN(2447, 1), FREQ2FBIN(2462, 1)},
        {FREQ2FBIN(2412, 1), FREQ2FBIN(2417, 1), FREQ2FBIN(2472, 1), 0x00},
        {FREQ2FBIN(2412, 1), FREQ2FBIN(2417, 1), FREQ2FBIN(2472, 1), 0x00},
        {FREQ2FBIN(2412, 1), FREQ2FBIN(2417, 1), FREQ2FBIN(2472, 1), 0x00},
        {FREQ2FBIN(2422, 1), FREQ2FBIN(2427, 1), FREQ2FBIN(2447, 1), FREQ2FBIN(2462, 1)},
    },
    {
        /* ctlPowerData_2G */
        {{CTL(60, 0), CTL(60, 1), CTL(60, 0), CTL(60, 0)}},
        {{CTL(60, 0), CTL(60, 1), CTL(60, 0), CTL(60, 0)}},
        {{CTL(60, 1), CTL(60, 0), CTL(60, 0), CTL(60, 1)}},
        {{CTL(60, 1), CTL(60, 0), CTL(60, 0), CTL(60, 0)}},
        {{CTL(60, 0), CTL(60, 1), CTL(60, 0), CTL(60, 0)}},
        {{CTL(60, 0), CTL(60, 1), CTL(60, 0), CTL(60, 0)}},
        {{CTL(60, 0), CTL(60, 1), CTL(60, 1), CTL(60, 0)}},
        {{CTL(60, 0), CTL(60, 1), CTL(60, 0), CTL(60, 0)}},
        {{CTL(60, 0), CTL(60, 1), CTL(60, 0), CTL(60, 0)}},
        {{CTL(60, 0), CTL(60, 1), CTL(60, 0), CTL(60, 0)}},
        {{CTL(60, 0), CTL(60, 1), CTL(60, 1), CTL(60, 1)}},
        {{CTL(60, 0), CTL(60, 1), CTL(60, 1), CTL(60, 1)}},
    },
    {
        /* modalHeader5G */
        0x110,                             /* antCtrlCommon */
        0x22222,                           /* antCtrlCommon2 */
        {0x0, 0x0, 0x0},                   /* antCtrlChain */
        {19, 25, 23},                      /* xatten1DB */
        {25, 25, 25},                      /* xatten1Margin */
        70,                                /* tempSlope */
        15,                                /* voltSlope */
        {0, 0, 0, 0, 0},                   /* spurChans */
        {-1, 0, 0},                        /* noiseFloorThreshCh */
        {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, /* reserved */
        0,                                 /* quick_drop */
        0,                                 /* xpaBiasLvl */
        14,                                /* txFrameToDataStart */
        14,                                /* txFrameToPaOn */
        3,                                 /* txClip */
        0,                                 /* antennaGain */
        45,                                /* switchSettling */
        -30,                               /* adcDesiredSize */
        0,                                 /* txEndToXpaOff */
        2,                                 /* txEndToRxOn */
        14,                                /* txFrameToXpaOn */
        28,                                /* thresh62 */
        0xcf0e0e0,                         /* papdRateMaskHt20 */
        0x6cf0e0e0,                        /* papdRateMaskHt40 */
        0x0,                               /* switchcomspdt */
        0,                                 /* xlna_bias_strength */
        {0, 0, 0, 0, 0, 0, 0},             /* futureModal */
    },
    {
        /* base_ext2 */
        72,           /* tempSlopeLow */
        105,          /* tempSlopeHigh */
        {16, 20, 16}, /* xatten1DBLow */
        {25, 25, 25}, /* xatten1MarginLow */
        {29, 32, 36}, /* xatten1DBHigh */
        {16, 16, 16}, /* xatten1MarginHigh */
    },
    {FREQ2FBIN(5180, 0), FREQ2FBIN(5220, 0), FREQ2FBIN(5320, 0), FREQ2FBIN(5400, 0),
     FREQ2FBIN(5500, 0), FREQ2FBIN(5600, 0), FREQ2FBIN(5700, 0),
     FREQ2FBIN(5785, 0)}, /* calFreqPier5G */
    {
        /* calPierData5G */
        {{0, 0, 0, 0, 0, 0},
         {0, 0, 0, 0, 0, 0},
         {0, 0, 0, 0, 0, 0},
         {0, 0, 0, 0, 0, 0},
         {0, 0, 0, 0, 0, 0},
         {0, 0, 0, 0, 0, 0},
         {0, 0, 0, 0, 0, 0},
         {0, 0, 0, 0, 0, 0}},
        {{0, 0, 0, 0, 0, 0},
         {0, 0, 0, 0, 0, 0},
         {0, 0, 0, 0, 0, 0},
         {0, 0, 0, 0, 0, 0},
         {0, 0, 0, 0, 0, 0},
         {0, 0, 0, 0, 0, 0},
         {0, 0, 0, 0, 0, 0},
         {0, 0, 0, 0, 0, 0}},
        {{0, 0, 0, 0, 0, 0},
         {0, 0, 0, 0, 0, 0},
         {0, 0, 0, 0, 0, 0},
         {0, 0, 0, 0, 0, 0},
         {0, 0, 0, 0, 0, 0},
         {0, 0, 0, 0, 0, 0},
         {0, 0, 0, 0, 0, 0},
         {0, 0, 0, 0, 0, 0}},
    },
    {FREQ2FBIN(5180, 0), FREQ2FBIN(5220, 0), FREQ2FBIN(5320, 0), FREQ2FBIN(5400, 0),
     FREQ2FBIN(5500, 0), FREQ2FBIN(5600, 0), FREQ2FBIN(5725, 0),
     FREQ2FBIN(5825, 0)}, /* calTarget_freqbin_5G */
    {FREQ2FBIN(5180, 0), FREQ2FBIN(5220, 0), FREQ2FBIN(5320, 0), FREQ2FBIN(5400, 0),
     FREQ2FBIN(5500, 0), FREQ2FBIN(5600, 0), FREQ2FBIN(5725, 0),
     FREQ2FBIN(5825, 0)}, /* calTarget_freqbin_5GHT20 */
    {FREQ2FBIN(5180, 0), FREQ2FBIN(5220, 0), FREQ2FBIN(5320, 0), FREQ2FBIN(5400, 0),
     FREQ2FBIN(5500, 0), FREQ2FBIN(5600, 0), FREQ2FBIN(5725, 0),
     FREQ2FBIN(5825, 0)}, /* calTarget_freqbin_5GHT40 */
    {
        /* calTargetPower5G */
        {{32, 32, 28, 26}},
        {{32, 32, 28, 26}},
        {{32, 32, 28, 26}},
        {{32, 32, 26, 24}},
        {{32, 32, 26, 24}},
        {{32, 32, 24, 22}},
        {{30, 30, 24, 22}},
        {{30, 30, 24, 22}},
    },
    {
        /* calTargetPower5GHT20 */
        {{32, 32, 32, 32, 28, 26, 32, 28, 26, 24, 24, 24, 22, 22}},
        {{32, 32, 32, 32, 28, 26, 32, 28, 26, 24, 24, 24, 22, 22}},
        {{32, 32, 32, 32, 28, 26, 32, 28, 26, 24, 24, 24, 22, 22}},
        {{32, 32, 32, 32, 28, 26, 32, 26, 24, 22, 22, 22, 20, 20}},
        {{32, 32, 32, 32, 28, 26, 32, 26, 24, 22, 20, 18, 16, 16}},
        {{32, 32, 32, 32, 28, 26, 32, 24, 20, 16, 18, 16, 14, 14}},
        {{30, 30, 30, 30, 28, 26, 30, 24, 20, 16, 18, 16, 14, 14}},
        {{30, 30, 30, 30, 28, 26, 30, 24, 20, 16, 18, 16, 14, 14}},
    },
    {
        /* calTargetPower5GHT40 */
        {{32, 32, 32, 30, 28, 26, 30, 28, 26, 24, 24, 24, 22, 22}},
        {{32, 32, 32, 30, 28, 26, 30, 28, 26, 24, 24, 24, 22, 22}},
        {{32, 32, 32, 30, 28, 26, 30, 28, 26, 24, 24, 24, 22, 22}},
        {{32, 32, 32, 30, 28, 26, 30, 26, 24, 22, 22, 22, 20, 20}},
        {{32, 32, 32, 30, 28, 26, 30, 26, 24, 22, 20, 18, 16, 16}},
        {{32, 32, 32, 30, 28, 26, 30, 22, 20, 16, 18, 16, 14, 14}},
        {{30, 30, 30, 30, 28, 26, 30, 22, 20, 16, 18, 16, 14, 14}},
        {{30, 30, 30, 30, 28, 26, 30, 22, 20, 16, 18, 16, 14, 14}},
    },
    {0x10, 0x16, 0x18, 0x40, 0x46, 0x48, 0x30, 0x36, 0x38}, /* ctlIndex_5G */
    {
        /* ctl_freqbin_5G */
        {FREQ2FBIN(5180, 0), FREQ2FBIN(5260, 0), FREQ2FBIN(5280, 0), FREQ2FBIN(5500, 0),
         FREQ2FBIN(5600, 0), FREQ2FBIN(5700, 0), FREQ2FBIN(5745, 0), FREQ2FBIN(5825, 0)},
        {FREQ2FBIN(5180, 0), FREQ2FBIN(5260, 0), FREQ2FBIN(5280, 0), FREQ2FBIN(5500, 0),
         FREQ2FBIN(5520, 0), FREQ2FBIN(5700, 0), FREQ2FBIN(5745, 0), FREQ2FBIN(5825, 0)},
        {FREQ2FBIN(5190, 0), FREQ2FBIN(5230, 0), FREQ2FBIN(5270, 0), FREQ2FBIN(5310, 0),
         FREQ2FBIN(5510, 0), FREQ2FBIN(5550, 0), FREQ2FBIN(5670, 0), FREQ2FBIN(5755, 0)},
        {FREQ2FBIN(5180, 0), FREQ2FBIN(5200, 0), FREQ2FBIN(5260, 0), FREQ2FBIN(5320, 0),
         FREQ2FBIN(5500, 0), FREQ2FBIN(5700, 0), 0xff, 0xff},
        {FREQ2FBIN(5180, 0), FREQ2FBIN(5260, 0), FREQ2FBIN(5500, 0), FREQ2FBIN(5700, 0), 0xff, 0xff,
         0xff, 0xff},
        {FREQ2FBIN(5190, 0), FREQ2FBIN(5270, 0), FREQ2FBIN(5310, 0), FREQ2FBIN(5510, 0),
         FREQ2FBIN(5590, 0), FREQ2FBIN(5670, 0), 0xff, 0xff},
        {FREQ2FBIN(5180, 0), FREQ2FBIN(5200, 0), FREQ2FBIN(5220, 0), FREQ2FBIN(5260, 0),
         FREQ2FBIN(5500, 0), FREQ2FBIN(5600, 0), FREQ2FBIN(5700, 0), FREQ2FBIN(5745, 0)},
        {FREQ2FBIN(5180, 0), FREQ2FBIN(5260, 0), FREQ2FBIN(5320, 0), FREQ2FBIN(5500, 0),
         FREQ2FBIN(5560, 0), FREQ2FBIN(5700, 0), FREQ2FBIN(5745, 0), FREQ2FBIN(5825, 0)},
        {FREQ2FBIN(5190, 0), FREQ2FBIN(5230, 0), FREQ2FBIN(5270, 0), FREQ2FBIN(5510, 0),
         FREQ2FBIN(5550, 0), FREQ2FBIN(5670, 0), FREQ2FBIN(5755, 0), FREQ2FBIN(5795, 0)},
    },
    {
        /* ctlPowerData_5G */
        {{CTL(60, 1), CTL(60, 1), CTL(60, 1), CTL(60, 1), CTL(60, 1), CTL(60, 1), CTL(60, 1),
          CTL(60, 0)}},
        {{CTL(60, 1), CTL(60, 1), CTL(60, 1), CTL(60, 1), CTL(60, 1), CTL(60, 1), CTL(60, 1),
          CTL(60, 0)}},
        {{CTL(60, 0), CTL(60, 1), CTL(60, 0), CTL(60, 1), CTL(60, 1), CTL(60, 1), CTL(60, 1),
          CTL(60, 1)}},
        {{CTL(60, 0), CTL(60, 1), CTL(60, 1), CTL(60, 0), CTL(60, 1), CTL(60, 0), CTL(60, 0),
          CTL(60, 0)}},
        {{CTL(60, 1), CTL(60, 1), CTL(60, 1), CTL(60, 0), CTL(60, 0), CTL(60, 0), CTL(60, 0),
          CTL(60, 0)}},
        {{CTL(60, 1), CTL(60, 1), CTL(60, 1), CTL(60, 1), CTL(60, 1), CTL(60, 0), CTL(60, 0),
          CTL(60, 0)}},
        {{CTL(60, 1), CTL(60, 1), CTL(60, 1), CTL(60, 1), CTL(60, 1), CTL(60, 1), CTL(60, 1),
          CTL(60, 1)}},
        {{CTL(60, 1), CTL(60, 1), CTL(60, 0), CTL(60, 1), CTL(60, 1), CTL(60, 1), CTL(60, 1),
          CTL(60, 0)}},
        {{CTL(60, 1), CTL(60, 0), CTL(60, 1), CTL(60, 1), CTL(60, 1), CTL(60, 1), CTL(60, 0),
          CTL(60, 1)}},
    },
};

static const struct ar9300_eeprom ar9300_h116 = {
    2,                                    /* eepromVersion */
    4,                                    /* templateVersion */
    {0x00, 0x03, 0x7f, 0x00, 0x00, 0x00}, /* macAddr */
    {0x68, 0x31, 0x31, 0x36, 0x2d, 0x30, 0x34, 0x31, 0x2d, 0x66,
     0x30, 0x30, 0x30, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, /* custData */
    {
        /* baseEepHeader */
        {0x0, 0x1f},  /* regDmn */
        0x33,         /* txrxMask */
        {0x03, 0x00}, /* opCapFlags */
        0,            /* rfSilent */
        0,            /* blueToothOptions */
        0,            /* deviceCap */
        5,            /* deviceType */
        0,            /* pwrTableOffset */
        {0, 0},       /* params_for_tuning_caps */
        0x0d,         /* featureEnable */
        0x00,         /* miscConfiguration */
        6,            /* eepromWriteEnableGpio */
        0,            /* wlanDisableGpio */
        8,            /* wlanLedGpio */
        255,          /* rxBandSelectGpio */
        0x10,         /* txrxgain */
        0x0,          /* swreg */
    },
    {
        /* modalHeader2G */
        0x110,                             /* antCtrlCommon */
        0x44444,                           /* antCtrlCommon2 */
        {0x10, 0x10, 0x10},                /* antCtrlChain */
        {31, 31, 31},                      /* xatten1DB */
        {18, 18, 18},                      /* xatten1Margin */
        25,                                /* tempSlope */
        0,                                 /* voltSlope */
        {164, 0, 0, 0, 0},                 /* spurChans */
        {-1, 0, 0},                        /* noiseFloorThreshCh */
        {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, /* reserved */
        0,                                 /* quick_drop */
        0,                                 /* xpaBiasLvl */
        14,                                /* txFrameToDataStart */
        14,                                /* txFrameToPaOn */
        3,                                 /* txClip */
        0,                                 /* antennaGain */
        44,                                /* switchSettling */
        -30,                               /* adcDesiredSize */
        0,                                 /* txEndToXpaOff */
        2,                                 /* txEndToRxOn */
        14,                                /* txFrameToXpaOn */
        28,                                /* thresh62 */
        0xc80c080,                         /* papdRateMaskHt20 */
        0x80c080,                          /* papdRateMaskHt40 */
        0x0,                               /* switchcomspdt */
        0,                                 /* xlna_bias_strength */
        {0, 0, 0, 0, 0, 0, 0},             /* futureModal */
    },
    {
        /* base_ext1 */
        0x00,                     /* ant_div_control */
        {0, 0},                   /* future */
        0x00,                     /* misc_enable */
        {0, 0, 0, 0, 0, 0, 0, 0}, /* tempslopextension */
        0,                        /* quick_drop_low */
        0,                        /* quick_drop_high */
    },
    {FREQ2FBIN(2412, 1), FREQ2FBIN(2437, 1), FREQ2FBIN(2462, 1)}, /* calFreqPier2G */
    {
        /* calPierData2G */
        {{0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0}},
        {{0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0}},
        {{0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0}},
    },
    {FREQ2FBIN(2412, 1), FREQ2FBIN(2472, 1)},                     /* calTarget_freqbin_Cck */
    {FREQ2FBIN(2412, 1), FREQ2FBIN(2437, 1), FREQ2FBIN(2472, 1)}, /* calTarget_freqbin_2G */
    {FREQ2FBIN(2412, 1), FREQ2FBIN(2437, 1), FREQ2FBIN(2472, 1)}, /* calTarget_freqbin_2GHT20 */
    {FREQ2FBIN(2412, 1), FREQ2FBIN(2437, 1), FREQ2FBIN(2472, 1)}, /* calTarget_freqbin_2GHT40 */
    {
        /* calTargetPowerCck */
        {{34, 34, 34, 34}},
        {{34, 34, 34, 34}},
    },
    {
        /* calTargetPower2G */
        {{34, 34, 32, 32}},
        {{34, 34, 32, 32}},
        {{34, 34, 32, 32}},
    },
    {
        /* calTargetPower2GHT20 */
        {{32, 32, 32, 32, 32, 30, 32, 32, 30, 28, 0, 0, 0, 0}},
        {{32, 32, 32, 32, 32, 30, 32, 32, 30, 28, 0, 0, 0, 0}},
        {{32, 32, 32, 32, 32, 30, 32, 32, 30, 28, 0, 0, 0, 0}},
    },
    {
        /* calTargetPower2GHT40 */
        {{30, 30, 30, 30, 30, 28, 30, 30, 28, 26, 0, 0, 0, 0}},
        {{30, 30, 30, 30, 30, 28, 30, 30, 28, 26, 0, 0, 0, 0}},
        {{30, 30, 30, 30, 30, 28, 30, 30, 28, 26, 0, 0, 0, 0}},
    },
    {0x11, 0x12, 0x15, 0x17, 0x41, 0x42, 0x45, 0x47, 0x31, 0x32, 0x35, 0x37}, /* ctlIndex_2G */
    {
        /* ctl_freqbin_2G */
        {FREQ2FBIN(2412, 1), FREQ2FBIN(2417, 1), FREQ2FBIN(2457, 1), FREQ2FBIN(2462, 1)},
        {FREQ2FBIN(2412, 1), FREQ2FBIN(2417, 1), FREQ2FBIN(2462, 1), 0xff},
        {FREQ2FBIN(2412, 1), FREQ2FBIN(2417, 1), FREQ2FBIN(2462, 1), 0xff},
        {FREQ2FBIN(2422, 1), FREQ2FBIN(2427, 1), FREQ2FBIN(2447, 1), FREQ2FBIN(2452, 1)},
        {FREQ2FBIN(2412, 1), FREQ2FBIN(2417, 1), FREQ2FBIN(2472, 1), FREQ2FBIN(2484, 1)},
        {FREQ2FBIN(2412, 1), FREQ2FBIN(2417, 1), FREQ2FBIN(2472, 1), 0x00},
        {FREQ2FBIN(2412, 1), FREQ2FBIN(2417, 1), FREQ2FBIN(2472, 1), 0x00},
        {FREQ2FBIN(2422, 1), FREQ2FBIN(2427, 1), FREQ2FBIN(2447, 1), FREQ2FBIN(2462, 1)},
        {FREQ2FBIN(2412, 1), FREQ2FBIN(2417, 1), FREQ2FBIN(2472, 1), 0x00},
        {FREQ2FBIN(2412, 1), FREQ2FBIN(2417, 1), FREQ2FBIN(2472, 1), 0x00},
        {FREQ2FBIN(2412, 1), FREQ2FBIN(2417, 1), FREQ2FBIN(2472, 1), 0x00},
        {FREQ2FBIN(2422, 1), FREQ2FBIN(2427, 1), FREQ2FBIN(2447, 1), FREQ2FBIN(2462, 1)},
    },
    {
        /* ctlPowerData_2G */
        {{CTL(60, 0), CTL(60, 1), CTL(60, 0), CTL(60, 0)}},
        {{CTL(60, 0), CTL(60, 1), CTL(60, 0), CTL(60, 0)}},
        {{CTL(60, 1), CTL(60, 0), CTL(60, 0), CTL(60, 1)}},
        {{CTL(60, 1), CTL(60, 0), CTL(60, 0), CTL(60, 0)}},
        {{CTL(60, 0), CTL(60, 1), CTL(60, 0), CTL(60, 0)}},
        {{CTL(60, 0), CTL(60, 1), CTL(60, 0), CTL(60, 0)}},
        {{CTL(60, 0), CTL(60, 1), CTL(60, 1), CTL(60, 0)}},
        {{CTL(60, 0), CTL(60, 1), CTL(60, 0), CTL(60, 0)}},
        {{CTL(60, 0), CTL(60, 1), CTL(60, 0), CTL(60, 0)}},
        {{CTL(60, 0), CTL(60, 1), CTL(60, 0), CTL(60, 0)}},
        {{CTL(60, 0), CTL(60, 1), CTL(60, 1), CTL(60, 1)}},
        {{CTL(60, 0), CTL(60, 1), CTL(60, 1), CTL(60, 1)}},
    },
    {
        /* modalHeader5G */
        0x220,                             /* antCtrlCommon */
        0x44444,                           /* antCtrlCommon2 */
        {0x150, 0x150, 0x150},             /* antCtrlChain */
        {25, 25, 25},                      /* xatten1DB */
        {20, 20, 20},                      /* xatten1Margin */
        70,                                /* tempSlope */
        0,                                 /* voltSlope */
        {0, 0, 0, 0, 0},                   /* spurChans */
        {-1, 0, 0},                        /* noiseFloorThreshCh */
        {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, /* reserved */
        0,                                 /* quick_drop */
        0,                                 /* xpaBiasLvl */
        14,                                /* txFrameToDataStart */
        14,                                /* txFrameToPaOn */
        3,                                 /* txClip */
        0,                                 /* antennaGain */
        45,                                /* switchSettling */
        -30,                               /* adcDesiredSize */
        0,                                 /* txEndToXpaOff */
        2,                                 /* txEndToRxOn */
        14,                                /* txFrameToXpaOn */
        28,                                /* thresh62 */
        0xcf0e0e0,                         /* papdRateMaskHt20 */
        0x6cf0e0e0,                        /* papdRateMaskHt40 */
        0x0,                               /* switchcomspdt */
        0,                                 /* xlna_bias_strength */
        {0, 0, 0, 0, 0, 0, 0},             /* futureModal */
    },
    {
        /* base_ext2 */
        35,        /* tempSlopeLow */
        50,        /* tempSlopeHigh */
        {0, 0, 0}, /* xatten1DBLow */
        {0, 0, 0}, /* xatten1MarginLow */
        {0, 0, 0}, /* xatten1DBHigh */
        {0, 0, 0}, /* xatten1MarginHigh */
    },
    {FREQ2FBIN(5160, 0), FREQ2FBIN(5220, 0), FREQ2FBIN(5320, 0), FREQ2FBIN(5400, 0),
     FREQ2FBIN(5500, 0), FREQ2FBIN(5600, 0), FREQ2FBIN(5700, 0),
     FREQ2FBIN(5785, 0)}, /* calFreqPier5G */
    {
        /* calPierData5G */
        {{0, 0, 0, 0, 0, 0},
         {0, 0, 0, 0, 0, 0},
         {0, 0, 0, 0, 0, 0},
         {0, 0, 0, 0, 0, 0},
         {0, 0, 0, 0, 0, 0},
         {0, 0, 0, 0, 0, 0},
         {0, 0, 0, 0, 0, 0},
         {0, 0, 0, 0, 0, 0}},
        {{0, 0, 0, 0, 0, 0},
         {0, 0, 0, 0, 0, 0},
         {0, 0, 0, 0, 0, 0},
         {0, 0, 0, 0, 0, 0},
         {0, 0, 0, 0, 0, 0},
         {0, 0, 0, 0, 0, 0},
         {0, 0, 0, 0, 0, 0},
         {0, 0, 0, 0, 0, 0}},
        {{0, 0, 0, 0, 0, 0},
         {0, 0, 0, 0, 0, 0},
         {0, 0, 0, 0, 0, 0},
         {0, 0, 0, 0, 0, 0},
         {0, 0, 0, 0, 0, 0},
         {0, 0, 0, 0, 0, 0},
         {0, 0, 0, 0, 0, 0},
         {0, 0, 0, 0, 0, 0}},
    },
    {FREQ2FBIN(5180, 0), FREQ2FBIN(5240, 0), FREQ2FBIN(5320, 0), FREQ2FBIN(5400, 0),
     FREQ2FBIN(5500, 0), FREQ2FBIN(5600, 0), FREQ2FBIN(5700, 0),
     FREQ2FBIN(5825, 0)}, /* calTarget_freqbin_5G */
    {FREQ2FBIN(5180, 0), FREQ2FBIN(5240, 0), FREQ2FBIN(5320, 0), FREQ2FBIN(5400, 0),
     FREQ2FBIN(5500, 0), FREQ2FBIN(5700, 0), FREQ2FBIN(5745, 0),
     FREQ2FBIN(5825, 0)}, /* calTarget_freqbin_5GHT20 */
    {FREQ2FBIN(5180, 0), FREQ2FBIN(5240, 0), FREQ2FBIN(5320, 0), FREQ2FBIN(5400, 0),
     FREQ2FBIN(5500, 0), FREQ2FBIN(5700, 0), FREQ2FBIN(5745, 0),
     FREQ2FBIN(5825, 0)}, /* calTarget_freqbin_5GHT40 */
    {
        /* calTargetPower5G */
        {{30, 30, 28, 24}},
        {{30, 30, 28, 24}},
        {{30, 30, 28, 24}},
        {{30, 30, 28, 24}},
        {{30, 30, 28, 24}},
        {{30, 30, 28, 24}},
        {{30, 30, 28, 24}},
        {{30, 30, 28, 24}},
    },
    {
        /* calTargetPower5GHT20 */
        {{30, 30, 30, 28, 24, 20, 30, 28, 24, 20, 0, 0, 0, 0}},
        {{30, 30, 30, 28, 24, 20, 30, 28, 24, 20, 0, 0, 0, 0}},
        {{30, 30, 30, 26, 22, 18, 30, 26, 22, 18, 0, 0, 0, 0}},
        {{30, 30, 30, 26, 22, 18, 30, 26, 22, 18, 0, 0, 0, 0}},
        {{30, 30, 30, 24, 20, 16, 30, 24, 20, 16, 0, 0, 0, 0}},
        {{30, 30, 30, 24, 20, 16, 30, 24, 20, 16, 0, 0, 0, 0}},
        {{30, 30, 30, 22, 18, 14, 30, 22, 18, 14, 0, 0, 0, 0}},
        {{30, 30, 30, 22, 18, 14, 30, 22, 18, 14, 0, 0, 0, 0}},
    },
    {
        /* calTargetPower5GHT40 */
        {{28, 28, 28, 26, 22, 18, 28, 26, 22, 18, 0, 0, 0, 0}},
        {{28, 28, 28, 26, 22, 18, 28, 26, 22, 18, 0, 0, 0, 0}},
        {{28, 28, 28, 24, 20, 16, 28, 24, 20, 16, 0, 0, 0, 0}},
        {{28, 28, 28, 24, 20, 16, 28, 24, 20, 16, 0, 0, 0, 0}},
        {{28, 28, 28, 22, 18, 14, 28, 22, 18, 14, 0, 0, 0, 0}},
        {{28, 28, 28, 22, 18, 14, 28, 22, 18, 14, 0, 0, 0, 0}},
        {{28, 28, 28, 20, 16, 12, 28, 20, 16, 12, 0, 0, 0, 0}},
        {{28, 28, 28, 20, 16, 12, 28, 20, 16, 12, 0, 0, 0, 0}},
    },
    {0x10, 0x16, 0x18, 0x40, 0x46, 0x48, 0x30, 0x36, 0x38}, /* ctlIndex_5G */
    {
        /* ctl_freqbin_5G */
        {FREQ2FBIN(5180, 0), FREQ2FBIN(5260, 0), FREQ2FBIN(5280, 0), FREQ2FBIN(5500, 0),
         FREQ2FBIN(5600, 0), FREQ2FBIN(5700, 0), FREQ2FBIN(5745, 0), FREQ2FBIN(5825, 0)},
        {FREQ2FBIN(5180, 0), FREQ2FBIN(5260, 0), FREQ2FBIN(5280, 0), FREQ2FBIN(5500, 0),
         FREQ2FBIN(5520, 0), FREQ2FBIN(5700, 0), FREQ2FBIN(5745, 0), FREQ2FBIN(5825, 0)},
        {FREQ2FBIN(5190, 0), FREQ2FBIN(5230, 0), FREQ2FBIN(5270, 0), FREQ2FBIN(5310, 0),
         FREQ2FBIN(5510, 0), FREQ2FBIN(5550, 0), FREQ2FBIN(5670, 0), FREQ2FBIN(5755, 0)},
        {FREQ2FBIN(5180, 0), FREQ2FBIN(5200, 0), FREQ2FBIN(5260, 0), FREQ2FBIN(5320, 0),
         FREQ2FBIN(5500, 0), FREQ2FBIN(5700, 0), 0xff, 0xff},
        {FREQ2FBIN(5180, 0), FREQ2FBIN(5260, 0), FREQ2FBIN(5500, 0), FREQ2FBIN(5700, 0), 0xff, 0xff,
         0xff, 0xff},
        {FREQ2FBIN(5190, 0), FREQ2FBIN(5270, 0), FREQ2FBIN(5310, 0), FREQ2FBIN(5510, 0),
         FREQ2FBIN(5590, 0), FREQ2FBIN(5670, 0), 0xff, 0xff},
        {FREQ2FBIN(5180, 0), FREQ2FBIN(5200, 0), FREQ2FBIN(5220, 0), FREQ2FBIN(5260, 0),
         FREQ2FBIN(5500, 0), FREQ2FBIN(5600, 0), FREQ2FBIN(5700, 0), FREQ2FBIN(5745, 0)},
        {FREQ2FBIN(5180, 0), FREQ2FBIN(5260, 0), FREQ2FBIN(5320, 0), FREQ2FBIN(5500, 0),
         FREQ2FBIN(5560, 0), FREQ2FBIN(5700, 0), FREQ2FBIN(5745, 0), FREQ2FBIN(5825, 0)},
        {FREQ2FBIN(5190, 0), FREQ2FBIN(5230, 0), FREQ2FBIN(5270, 0), FREQ2FBIN(5510, 0),
         FREQ2FBIN(5550, 0), FREQ2FBIN(5670, 0), FREQ2FBIN(5755, 0), FREQ2FBIN(5795, 0)},
    },
    {
        /* ctlPowerData_5G */
        {{CTL(60, 1), CTL(60, 1), CTL(60, 1), CTL(60, 1), CTL(60, 1), CTL(60, 1), CTL(60, 1),
          CTL(60, 0)}},
        {{CTL(60, 1), CTL(60, 1), CTL(60, 1), CTL(60, 1), CTL(60, 1), CTL(60, 1), CTL(60, 1),
          CTL(60, 0)}},
        {{CTL(60, 0), CTL(60, 1), CTL(60, 0), CTL(60, 1), CTL(60, 1), CTL(60, 1), CTL(60, 1),
          CTL(60, 1)}},
        {{CTL(60, 0), CTL(60, 1), CTL(60, 1), CTL(60, 0), CTL(60, 1), CTL(60, 0), CTL(60, 0),
          CTL(60, 0)}},
        {{CTL(60, 1), CTL(60, 1), CTL(60, 1), CTL(60, 0), CTL(60, 0), CTL(60, 0), CTL(60, 0),
          CTL(60, 0)}},
        {{CTL(60, 1), CTL(60, 1), CTL(60, 1), CTL(60, 1), CTL(60, 1), CTL(60, 0), CTL(60, 0),
          CTL(60, 0)}},
        {{CTL(60, 1), CTL(60, 1), CTL(60, 1), CTL(60, 1), CTL(60, 1), CTL(60, 1), CTL(60, 1),
          CTL(60, 1)}},
        {{CTL(60, 1), CTL(60, 1), CTL(60, 0), CTL(60, 1), CTL(60, 1), CTL(60, 1), CTL(60, 1),
          CTL(60, 0)}},
        {{CTL(60, 1), CTL(60, 0), CTL(60, 1), CTL(60, 1), CTL(60, 1), CTL(60, 1), CTL(60, 0),
          CTL(60, 1)}},
    },
};

static const struct ar9300_eeprom ar9300_h112 = {
    2,                                    /* eepromVersion */
    3,                                    /* templateVersion */
    {0x00, 0x03, 0x7f, 0x00, 0x00, 0x00}, /* macAddr */
    {0x68, 0x31, 0x31, 0x32, 0x2d, 0x32, 0x34, 0x31, 0x2d, 0x66,
     0x30, 0x30, 0x30, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, /* custData */
    {
        /* baseEepHeader */
        {0x0, 0x1f},  /* regDmn */
        0x77,         /* txrxMask */
        {0x03, 0x00}, /* opCapFlags */
        0,            /* rfSilent */
        0,            /* blueToothOptions */
        0,            /* deviceCap */
        5,            /* deviceType */
        0,            /* pwrTableOffset */
        {0, 0},       /* params_for_tuning_caps */
        0x0d,         /* featureEnable */
        0x00,         /* miscConfiguration */
        6,            /* eepromWriteEnableGpio */
        0,            /* wlanDisableGpio */
        8,            /* wlanLedGpio */
        255,          /* rxBandSelectGpio */
        0x10,         /* txrxgain */
        0x0,          /* swreg */
    },
    {
        /* modalHeader2G */
        0x110,                             /* antCtrlCommon */
        0x44444,                           /* antCtrlCommon2 */
        {0x150, 0x150, 0x150},             /* antCtrlChain */
        {0, 0, 0},                         /* xatten1DB */
        {0, 0, 0},                         /* xatten1Margin */
        25,                                /* tempSlope */
        0,                                 /* voltSlope */
        {164, 0, 0, 0, 0},                 /* spurChans */
        {-1, 0, 0},                        /* noiseFloorThreshCh */
        {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, /* reserved */
        0,                                 /* quick_drop */
        0,                                 /* xpaBiasLvl */
        14,                                /* txFrameToDataStart */
        14,                                /* txFrameToPaOn */
        3,                                 /* txClip */
        0,                                 /* antennaGain */
        44,                                /* switchSettling */
        -30,                               /* adcDesiredSize */
        0,                                 /* txEndToXpaOff */
        2,                                 /* txEndToRxOn */
        14,                                /* txFrameToXpaOn */
        28,                                /* thresh62 */
        0xc80c080,                         /* papdRateMaskHt20 */
        0x80c080,                          /* papdRateMaskHt40 */
        0x0,                               /* switchcomspdt */
        0,                                 /* xlna_bias_strength */
        {0, 0, 0, 0, 0, 0, 0},             /* futureModal */
    },
    {
        /* base_ext1 */
        0x00,                     /* ant_div_control */
        {0, 0},                   /* future */
        0x00,                     /* misc_enable */
        {0, 0, 0, 0, 0, 0, 0, 0}, /* tempslopextension */
        0,                        /* quick_drop_low */
        0,                        /* quick_drop_high */
    },
    {FREQ2FBIN(2412, 1), FREQ2FBIN(2437, 1), FREQ2FBIN(2462, 1)}, /* calFreqPier2G */
    {
        /* calPierData2G */
        {{0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0}},
        {{0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0}},
        {{0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0}},
    },
    {FREQ2FBIN(2412, 1), FREQ2FBIN(2472, 1)},                     /* calTarget_freqbin_Cck */
    {FREQ2FBIN(2412, 1), FREQ2FBIN(2437, 1), FREQ2FBIN(2472, 1)}, /* calTarget_freqbin_2G */
    {FREQ2FBIN(2412, 1), FREQ2FBIN(2437, 1), FREQ2FBIN(2472, 1)}, /* calTarget_freqbin_2GHT20 */
    {FREQ2FBIN(2412, 1), FREQ2FBIN(2437, 1), FREQ2FBIN(2472, 1)}, /* calTarget_freqbin_2GHT40 */
    {
        /* calTargetPowerCck */
        {{34, 34, 34, 34}},
        {{34, 34, 34, 34}},
    },
    {
        /* calTargetPower2G */
        {{34, 34, 32, 32}},
        {{34, 34, 32, 32}},
        {{34, 34, 32, 32}},
    },
    {
        /* calTargetPower2GHT20 */
        {{32, 32, 32, 32, 32, 30, 32, 32, 30, 28, 28, 28, 28, 24}},
        {{32, 32, 32, 32, 32, 30, 32, 32, 30, 28, 28, 28, 28, 24}},
        {{32, 32, 32, 32, 32, 30, 32, 32, 30, 28, 28, 28, 28, 24}},
    },
    {
        /* calTargetPower2GHT40 */
        {{30, 30, 30, 30, 30, 28, 30, 30, 28, 26, 26, 26, 26, 22}},
        {{30, 30, 30, 30, 30, 28, 30, 30, 28, 26, 26, 26, 26, 22}},
        {{30, 30, 30, 30, 30, 28, 30, 30, 28, 26, 26, 26, 26, 22}},
    },
    {0x11, 0x12, 0x15, 0x17, 0x41, 0x42, 0x45, 0x47, 0x31, 0x32, 0x35, 0x37}, /* ctlIndex_2G */
    {
        /* ctl_freqbin_2G */
        {FREQ2FBIN(2412, 1), FREQ2FBIN(2417, 1), FREQ2FBIN(2457, 1), FREQ2FBIN(2462, 1)},
        {FREQ2FBIN(2412, 1), FREQ2FBIN(2417, 1), FREQ2FBIN(2462, 1), 0xff},
        {FREQ2FBIN(2412, 1), FREQ2FBIN(2417, 1), FREQ2FBIN(2462, 1), 0xff},
        {FREQ2FBIN(2422, 1), FREQ2FBIN(2427, 1), FREQ2FBIN(2447, 1), FREQ2FBIN(2452, 1)},
        {FREQ2FBIN(2412, 1), FREQ2FBIN(2417, 1), FREQ2FBIN(2472, 1), FREQ2FBIN(2484, 1)},
        {FREQ2FBIN(2412, 1), FREQ2FBIN(2417, 1), FREQ2FBIN(2472, 1), 0x00},
        {FREQ2FBIN(2412, 1), FREQ2FBIN(2417, 1), FREQ2FBIN(2472, 1), 0x00},
        {FREQ2FBIN(2422, 1), FREQ2FBIN(2427, 1), FREQ2FBIN(2447, 1), FREQ2FBIN(2462, 1)},
        {FREQ2FBIN(2412, 1), FREQ2FBIN(2417, 1), FREQ2FBIN(2472, 1), 0x00},
        {FREQ2FBIN(2412, 1), FREQ2FBIN(2417, 1), FREQ2FBIN(2472, 1), 0x00},
        {FREQ2FBIN(2412, 1), FREQ2FBIN(2417, 1), FREQ2FBIN(2472, 1), 0x00},
        {FREQ2FBIN(2422, 1), FREQ2FBIN(2427, 1), FREQ2FBIN(2447, 1), FREQ2FBIN(2462, 1)},
    },
    {
        /* ctlPowerData_2G */
        {{CTL(60, 0), CTL(60, 1), CTL(60, 0), CTL(60, 0)}},
        {{CTL(60, 0), CTL(60, 1), CTL(60, 0), CTL(60, 0)}},
        {{CTL(60, 1), CTL(60, 0), CTL(60, 0), CTL(60, 1)}},
        {{CTL(60, 1), CTL(60, 0), CTL(60, 0), CTL(60, 0)}},
        {{CTL(60, 0), CTL(60, 1), CTL(60, 0), CTL(60, 0)}},
        {{CTL(60, 0), CTL(60, 1), CTL(60, 0), CTL(60, 0)}},
        {{CTL(60, 0), CTL(60, 1), CTL(60, 1), CTL(60, 0)}},
        {{CTL(60, 0), CTL(60, 1), CTL(60, 0), CTL(60, 0)}},
        {{CTL(60, 0), CTL(60, 1), CTL(60, 0), CTL(60, 0)}},
        {{CTL(60, 0), CTL(60, 1), CTL(60, 0), CTL(60, 0)}},
        {{CTL(60, 0), CTL(60, 1), CTL(60, 1), CTL(60, 1)}},
        {{CTL(60, 0), CTL(60, 1), CTL(60, 1), CTL(60, 1)}},
    },
    {
        /* modalHeader5G */
        0x220,                             /* antCtrlCommon */
        0x44444,                           /* antCtrlCommon2 */
        {0x150, 0x150, 0x150},             /* antCtrlChain */
        {0, 0, 0},                         /* xatten1DB */
        {0, 0, 0},                         /* xatten1Margin */
        45,                                /* tempSlope */
        0,                                 /* voltSlope */
        {0, 0, 0, 0, 0},                   /* spurChans */
        {-1, 0, 0},                        /* noiseFloorThreshCh */
        {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, /* reserved */
        0,                                 /* quick_drop */
        0,                                 /* xpaBiasLvl */
        14,                                /* txFrameToDataStart */
        14,                                /* txFrameToPaOn */
        3,                                 /* txClip */
        0,                                 /* antennaGain */
        45,                                /* switchSettling */
        -30,                               /* adcDesiredSize */
        0,                                 /* txEndToXpaOff */
        2,                                 /* txEndToRxOn */
        14,                                /* txFrameToXpaOn */
        28,                                /* thresh62 */
        0xcf0e0e0,                         /* papdRateMaskHt20 */
        0x6cf0e0e0,                        /* papdRateMaskHt40 */
        0x0,                               /* switchcomspdt */
        0,                                 /* xlna_bias_strength */
        {0, 0, 0, 0, 0, 0, 0},             /* futureModal */
    },
    {
        /* base_ext2 */
        40,        /* tempSlopeLow */
        50,        /* tempSlopeHigh */
        {0, 0, 0}, /* xatten1DBLow */
        {0, 0, 0}, /* xatten1MarginLow */
        {0, 0, 0}, /* xatten1DBHigh */
        {0, 0, 0}, /* xatten1MarginHigh */
    },
    {FREQ2FBIN(5180, 0), FREQ2FBIN(5220, 0), FREQ2FBIN(5320, 0), FREQ2FBIN(5400, 0),
     FREQ2FBIN(5500, 0), FREQ2FBIN(5600, 0), FREQ2FBIN(5700, 0),
     FREQ2FBIN(5785, 0)}, /* calFreqPier5G */
    {
        /* calPierData5G */
        {{0, 0, 0, 0, 0, 0},
         {0, 0, 0, 0, 0, 0},
         {0, 0, 0, 0, 0, 0},
         {0, 0, 0, 0, 0, 0},
         {0, 0, 0, 0, 0, 0},
         {0, 0, 0, 0, 0, 0},
         {0, 0, 0, 0, 0, 0},
         {0, 0, 0, 0, 0, 0}},
        {{0, 0, 0, 0, 0, 0},
         {0, 0, 0, 0, 0, 0},
         {0, 0, 0, 0, 0, 0},
         {0, 0, 0, 0, 0, 0},
         {0, 0, 0, 0, 0, 0},
         {0, 0, 0, 0, 0, 0},
         {0, 0, 0, 0, 0, 0},
         {0, 0, 0, 0, 0, 0}},
        {{0, 0, 0, 0, 0, 0},
         {0, 0, 0, 0, 0, 0},
         {0, 0, 0, 0, 0, 0},
         {0, 0, 0, 0, 0, 0},
         {0, 0, 0, 0, 0, 0},
         {0, 0, 0, 0, 0, 0},
         {0, 0, 0, 0, 0, 0},
         {0, 0, 0, 0, 0, 0}},
    },
    {FREQ2FBIN(5180, 0), FREQ2FBIN(5240, 0), FREQ2FBIN(5320, 0), FREQ2FBIN(5400, 0),
     FREQ2FBIN(5500, 0), FREQ2FBIN(5600, 0), FREQ2FBIN(5700, 0),
     FREQ2FBIN(5825, 0)}, /* calTarget_freqbin_5G */
    {FREQ2FBIN(5180, 0), FREQ2FBIN(5240, 0), FREQ2FBIN(5320, 0), FREQ2FBIN(5400, 0),
     FREQ2FBIN(5500, 0), FREQ2FBIN(5700, 0), FREQ2FBIN(5745, 0),
     FREQ2FBIN(5825, 0)}, /* calTarget_freqbin_5GHT20 */
    {FREQ2FBIN(5180, 0), FREQ2FBIN(5240, 0), FREQ2FBIN(5320, 0), FREQ2FBIN(5400, 0),
     FREQ2FBIN(5500, 0), FREQ2FBIN(5700, 0), FREQ2FBIN(5745, 0),
     FREQ2FBIN(5825, 0)}, /* calTarget_freqbin_5GHT40 */
    {
        /* calTargetPower5G */
        {{30, 30, 28, 24}},
        {{30, 30, 28, 24}},
        {{30, 30, 28, 24}},
        {{30, 30, 28, 24}},
        {{30, 30, 28, 24}},
        {{30, 30, 28, 24}},
        {{30, 30, 28, 24}},
        {{30, 30, 28, 24}},
    },
    {
        /* calTargetPower5GHT20 */
        {{30, 30, 30, 28, 24, 20, 30, 28, 24, 20, 20, 20, 20, 16}},
        {{30, 30, 30, 28, 24, 20, 30, 28, 24, 20, 20, 20, 20, 16}},
        {{30, 30, 30, 26, 22, 18, 30, 26, 22, 18, 18, 18, 18, 16}},
        {{30, 30, 30, 26, 22, 18, 30, 26, 22, 18, 18, 18, 18, 16}},
        {{30, 30, 30, 24, 20, 16, 30, 24, 20, 16, 16, 16, 16, 14}},
        {{30, 30, 30, 24, 20, 16, 30, 24, 20, 16, 16, 16, 16, 14}},
        {{30, 30, 30, 22, 18, 14, 30, 22, 18, 14, 14, 14, 14, 12}},
        {{30, 30, 30, 22, 18, 14, 30, 22, 18, 14, 14, 14, 14, 12}},
    },
    {
        /* calTargetPower5GHT40 */
        {{28, 28, 28, 26, 22, 18, 28, 26, 22, 18, 18, 18, 18, 14}},
        {{28, 28, 28, 26, 22, 18, 28, 26, 22, 18, 18, 18, 18, 14}},
        {{28, 28, 28, 24, 20, 16, 28, 24, 20, 16, 16, 16, 16, 12}},
        {{28, 28, 28, 24, 20, 16, 28, 24, 20, 16, 16, 16, 16, 12}},
        {{28, 28, 28, 22, 18, 14, 28, 22, 18, 14, 14, 14, 14, 10}},
        {{28, 28, 28, 22, 18, 14, 28, 22, 18, 14, 14, 14, 14, 10}},
        {{28, 28, 28, 20, 16, 12, 28, 20, 16, 12, 12, 12, 12, 8}},
        {{28, 28, 28, 20, 16, 12, 28, 20, 16, 12, 12, 12, 12, 8}},
    },
    {0x10, 0x16, 0x18, 0x40, 0x46, 0x48, 0x30, 0x36, 0x38}, /* ctlIndex_5G */
    {
        /* ctl_freqbin_5G */
        {FREQ2FBIN(5180, 0), FREQ2FBIN(5260, 0), FREQ2FBIN(5280, 0), FREQ2FBIN(5500, 0),
         FREQ2FBIN(5600, 0), FREQ2FBIN(5700, 0), FREQ2FBIN(5745, 0), FREQ2FBIN(5825, 0)},
        {FREQ2FBIN(5180, 0), FREQ2FBIN(5260, 0), FREQ2FBIN(5280, 0), FREQ2FBIN(5500, 0),
         FREQ2FBIN(5520, 0), FREQ2FBIN(5700, 0), FREQ2FBIN(5745, 0), FREQ2FBIN(5825, 0)},
        {FREQ2FBIN(5190, 0), FREQ2FBIN(5230, 0), FREQ2FBIN(5270, 0), FREQ2FBIN(5310, 0),
         FREQ2FBIN(5510, 0), FREQ2FBIN(5550, 0), FREQ2FBIN(5670, 0), FREQ2FBIN(5755, 0)},
        {FREQ2FBIN(5180, 0), FREQ2FBIN(5200, 0), FREQ2FBIN(5260, 0), FREQ2FBIN(5320, 0),
         FREQ2FBIN(5500, 0), FREQ2FBIN(5700, 0), 0xff, 0xff},
        {FREQ2FBIN(5180, 0), FREQ2FBIN(5260, 0), FREQ2FBIN(5500, 0), FREQ2FBIN(5700, 0), 0xff, 0xff,
         0xff, 0xff},
        {FREQ2FBIN(5190, 0), FREQ2FBIN(5270, 0), FREQ2FBIN(5310, 0), FREQ2FBIN(5510, 0),
         FREQ2FBIN(5590, 0), FREQ2FBIN(5670, 0), 0xff, 0xff},
        {FREQ2FBIN(5180, 0), FREQ2FBIN(5200, 0), FREQ2FBIN(5220, 0), FREQ2FBIN(5260, 0),
         FREQ2FBIN(5500, 0), FREQ2FBIN(5600, 0), FREQ2FBIN(5700, 0), FREQ2FBIN(5745, 0)},
        {FREQ2FBIN(5180, 0), FREQ2FBIN(5260, 0), FREQ2FBIN(5320, 0), FREQ2FBIN(5500, 0),
         FREQ2FBIN(5560, 0), FREQ2FBIN(5700, 0), FREQ2FBIN(5745, 0), FREQ2FBIN(5825, 0)},
        {FREQ2FBIN(5190, 0), FREQ2FBIN(5230, 0), FREQ2FBIN(5270, 0), FREQ2FBIN(5510, 0),
         FREQ2FBIN(5550, 0), FREQ2FBIN(5670, 0), FREQ2FBIN(5755, 0), FREQ2FBIN(5795, 0)},
    },
    {
        /* ctlPowerData_5G */
        {{CTL(60, 1), CTL(60, 1), CTL(60, 1), CTL(60, 1), CTL(60, 1), CTL(60, 1), CTL(60, 1),
          CTL(60, 0)}},
        {{CTL(60, 1), CTL(60, 1), CTL(60, 1), CTL(60, 1), CTL(60, 1), CTL(60, 1), CTL(60, 1),
          CTL(60, 0)}},
        {{CTL(60, 0), CTL(60, 1), CTL(60, 0), CTL(60, 1), CTL(60, 1), CTL(60, 1), CTL(60, 1),
          CTL(60, 1)}},
        {{CTL(60, 0), CTL(60, 1), CTL(60, 1), CTL(60, 0), CTL(60, 1), CTL(60, 0), CTL(60, 0),
          CTL(60, 0)}},
        {{CTL(60, 1), CTL(60, 1), CTL(60, 1), CTL(60, 0), CTL(60, 0), CTL(60, 0), CTL(60, 0),
          CTL(60, 0)}},
        {{CTL(60, 1), CTL(60, 1), CTL(60, 1), CTL(60, 1), CTL(60, 1), CTL(60, 0), CTL(60, 0),
          CTL(60, 0)}},
        {{CTL(60, 1), CTL(60, 1), CTL(60, 1), CTL(60, 1), CTL(60, 1), CTL(60, 1), CTL(60, 1),
          CTL(60, 1)}},
        {{CTL(60, 1), CTL(60, 1), CTL(60, 0), CTL(60, 1), CTL(60, 1), CTL(60, 1), CTL(60, 1),
          CTL(60, 0)}},
        {{CTL(60, 1), CTL(60, 0), CTL(60, 1), CTL(60, 1), CTL(60, 1), CTL(60, 1), CTL(60, 0),
          CTL(60, 1)}},
    },
};

static const struct ar9300_eeprom ar9300_x113 = {
    2,                                    /* eepromVersion */
    6,                                    /* templateVersion */
    {0x00, 0x03, 0x7f, 0x00, 0x00, 0x00}, /* macAddr */
    {0x78, 0x31, 0x31, 0x33, 0x2d, 0x30, 0x32, 0x33, 0x2d, 0x66,
     0x30, 0x30, 0x30, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, /* custData */
    {
        /* baseEepHeader */
        {0x0, 0x1f},  /* regDmn */
        0x77,         /* txrxMask */
        {0x01, 0x00}, /* opCapFlags */
        0,            /* rfSilent */
        0,            /* blueToothOptions */
        0,            /* deviceCap */
        5,            /* deviceType */
        0,            /* pwrTableOffset */
        {0, 0},       /* params_for_tuning_caps */
        0x0d,         /* featureEnable */
        0x00,         /* miscConfiguration */
        6,            /* eepromWriteEnableGpio */
        0,            /* wlanDisableGpio */
        8,            /* wlanLedGpio */
        255,          /* rxBandSelectGpio */
        0x21,         /* txrxgain */
        0x0,          /* swreg */
    },
    {
        /* modalHeader2G */
        0x110,                             /* antCtrlCommon */
        0x44444,                           /* antCtrlCommon2 */
        {0x150, 0x150, 0x150},             /* antCtrlChain */
        {0, 0, 0},                         /* xatten1DB */
        {0, 0, 0},                         /* xatten1Margin */
        25,                                /* tempSlope */
        0,                                 /* voltSlope */
        {164, 0, 0, 0, 0},                 /* spurChans */
        {-1, 0, 0},                        /* noiseFloorThreshCh */
        {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, /* reserved */
        0,                                 /* quick_drop */
        0,                                 /* xpaBiasLvl */
        14,                                /* txFrameToDataStart */
        14,                                /* txFrameToPaOn */
        3,                                 /* txClip */
        0,                                 /* antennaGain */
        44,                                /* switchSettling */
        -30,                               /* adcDesiredSize */
        0,                                 /* txEndToXpaOff */
        2,                                 /* txEndToRxOn */
        14,                                /* txFrameToXpaOn */
        28,                                /* thresh62 */
        0xc80c080,                         /* papdRateMaskHt20 */
        0x80c080,                          /* papdRateMaskHt40 */
        0x0,                               /* switchcomspdt */
        0,                                 /* xlna_bias_strength */
        {0, 0, 0, 0, 0, 0, 0},             /* futureModal */
    },
    {
        /* base_ext1 */
        0x00,                     /* ant_div_control */
        {0, 0},                   /* future */
        0x00,                     /* misc_enable */
        {0, 0, 0, 0, 0, 0, 0, 0}, /* tempslopextension */
        0,                        /* quick_drop_low */
        0,                        /* quick_drop_high */
    },
    {FREQ2FBIN(2412, 1), FREQ2FBIN(2437, 1), FREQ2FBIN(2472, 1)}, /* calFreqPier2G */
    {
        /* calPierData2G */
        {{0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0}},
        {{0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0}},
        {{0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0}},
    },
    {FREQ2FBIN(2412, 1), FREQ2FBIN(2472, 1)},                     /* calTarget_freqbin_Cck */
    {FREQ2FBIN(2412, 1), FREQ2FBIN(2437, 1), FREQ2FBIN(2472, 1)}, /* calTarget_freqbin_2G */
    {FREQ2FBIN(2412, 1), FREQ2FBIN(2437, 1), FREQ2FBIN(2472, 1)}, /* calTarget_freqbin_2GHT20 */
    {FREQ2FBIN(2412, 1), FREQ2FBIN(2437, 1), FREQ2FBIN(2472, 1)}, /* calTarget_freqbin_2GHT40 */
    {
        /* calTargetPowerCck */
        {{34, 34, 34, 34}},
        {{34, 34, 34, 34}},
    },
    {
        /* calTargetPower2G */
        {{34, 34, 32, 32}},
        {{34, 34, 32, 32}},
        {{34, 34, 32, 32}},
    },
    {
        /* calTargetPower2GHT20 */
        {{32, 32, 32, 32, 32, 28, 32, 32, 30, 28, 0, 0, 0, 0}},
        {{32, 32, 32, 32, 32, 28, 32, 32, 30, 28, 0, 0, 0, 0}},
        {{32, 32, 32, 32, 32, 28, 32, 32, 30, 28, 0, 0, 0, 0}},
    },
    {
        /* calTargetPower2GHT40 */
        {{30, 30, 30, 30, 30, 28, 30, 30, 28, 26, 0, 0, 0, 0}},
        {{30, 30, 30, 30, 30, 28, 30, 30, 28, 26, 0, 0, 0, 0}},
        {{30, 30, 30, 30, 30, 28, 30, 30, 28, 26, 0, 0, 0, 0}},
    },
    {0x11, 0x12, 0x15, 0x17, 0x41, 0x42, 0x45, 0x47, 0x31, 0x32, 0x35, 0x37}, /* ctlIndex_2G */
    {
        /* ctl_freqbin_2G */
        {FREQ2FBIN(2412, 1), FREQ2FBIN(2417, 1), FREQ2FBIN(2457, 1), FREQ2FBIN(2462, 1)},
        {FREQ2FBIN(2412, 1), FREQ2FBIN(2417, 1), FREQ2FBIN(2462, 1), 0xff},
        {FREQ2FBIN(2412, 1), FREQ2FBIN(2417, 1), FREQ2FBIN(2462, 1), 0xff},
        {FREQ2FBIN(2422, 1), FREQ2FBIN(2427, 1), FREQ2FBIN(2447, 1), FREQ2FBIN(2452, 1)},
        {FREQ2FBIN(2412, 1), FREQ2FBIN(2417, 1), FREQ2FBIN(2472, 1), FREQ2FBIN(2484, 1)},
        {FREQ2FBIN(2412, 1), FREQ2FBIN(2417, 1), FREQ2FBIN(2472, 1), 0x00},
        {FREQ2FBIN(2412, 1), FREQ2FBIN(2417, 1), FREQ2FBIN(2472, 1), 0x00},
        {FREQ2FBIN(2422, 1), FREQ2FBIN(2427, 1), FREQ2FBIN(2447, 1), FREQ2FBIN(2462, 1)},
        {FREQ2FBIN(2412, 1), FREQ2FBIN(2417, 1), FREQ2FBIN(2472, 1), 0x00},
        {FREQ2FBIN(2412, 1), FREQ2FBIN(2417, 1), FREQ2FBIN(2472, 1), 0x00},
        {FREQ2FBIN(2412, 1), FREQ2FBIN(2417, 1), FREQ2FBIN(2472, 1), 0x00},
        {FREQ2FBIN(2422, 1), FREQ2FBIN(2427, 1), FREQ2FBIN(2447, 1), FREQ2FBIN(2462, 1)},
    },
    {
        /* ctlPowerData_2G */
        {{CTL(60, 0), CTL(60, 1), CTL(60, 0), CTL(60, 0)}},
        {{CTL(60, 0), CTL(60, 1), CTL(60, 0), CTL(60, 0)}},
        {{CTL(60, 1), CTL(60, 0), CTL(60, 0), CTL(60, 1)}},
        {{CTL(60, 1), CTL(60, 0), CTL(60, 0), CTL(60, 0)}},
        {{CTL(60, 0), CTL(60, 1), CTL(60, 0), CTL(60, 0)}},
        {{CTL(60, 0), CTL(60, 1), CTL(60, 0), CTL(60, 0)}},
        {{CTL(60, 0), CTL(60, 1), CTL(60, 1), CTL(60, 0)}},
        {{CTL(60, 0), CTL(60, 1), CTL(60, 0), CTL(60, 0)}},
        {{CTL(60, 0), CTL(60, 1), CTL(60, 0), CTL(60, 0)}},
        {{CTL(60, 0), CTL(60, 1), CTL(60, 0), CTL(60, 0)}},
        {{CTL(60, 0), CTL(60, 1), CTL(60, 1), CTL(60, 1)}},
        {{CTL(60, 0), CTL(60, 1), CTL(60, 1), CTL(60, 1)}},
    },
    {
        /* modalHeader5G */
        0x220,                             /* antCtrlCommon */
        0x11111,                           /* antCtrlCommon2 */
        {0x150, 0x150, 0x150},             /* antCtrlChain */
        {0, 0, 0},                         /* xatten1DB */
        {0, 0, 0},                         /* xatten1Margin */
        68,                                /* tempSlope */
        0,                                 /* voltSlope */
        {140, 0, 0, 0, 0},                 /* spurChans */
        {-1, 0, 0},                        /* noiseFloorThreshCh */
        {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, /* reserved */
        0,                                 /* quick_drop */
        15,                                /* xpaBiasLvl */
        14,                                /* txFrameToDataStart */
        14,                                /* txFrameToPaOn */
        3,                                 /* txClip */
        0,                                 /* antennaGain */
        45,                                /* switchSettling */
        -30,                               /* adcDesiredSize */
        0,                                 /* txEndToXpaOff */
        2,                                 /* txEndToRxOn */
        14,                                /* txFrameToXpaOn */
        28,                                /* thresh62 */
        0xcf0e0e0,                         /* papdRateMaskHt20 */
        0x6cf0e0e0,                        /* papdRateMaskHt40 */
        0x0,                               /* switchcomspdt */
        0,                                 /* xlna_bias_strength */
        {0, 0, 0, 0, 0, 0, 0},             /* futureModal */
    },
    {
        /* base_ext2 */
        72,        /* tempSlopeLow */
        105,       /* tempSlopeHigh */
        {0, 0, 0}, /* xatten1DBLow */
        {0, 0, 0}, /* xatten1MarginLow */
        {0, 0, 0}, /* xatten1DBHigh */
        {0, 0, 0}, /* xatten1MarginHigh */
    },
    {FREQ2FBIN(5180, 0), FREQ2FBIN(5240, 0), FREQ2FBIN(5320, 0), FREQ2FBIN(5400, 0),
     FREQ2FBIN(5500, 0), FREQ2FBIN(5600, 0), FREQ2FBIN(5745, 0),
     FREQ2FBIN(5785, 0)}, /* calFreqPier5G */
    {
        /* calPierData5G */
        {{0, 0, 0, 0, 0, 0},
         {0, 0, 0, 0, 0, 0},
         {0, 0, 0, 0, 0, 0},
         {0, 0, 0, 0, 0, 0},
         {0, 0, 0, 0, 0, 0},
         {0, 0, 0, 0, 0, 0},
         {0, 0, 0, 0, 0, 0},
         {0, 0, 0, 0, 0, 0}},
        {{0, 0, 0, 0, 0, 0},
         {0, 0, 0, 0, 0, 0},
         {0, 0, 0, 0, 0, 0},
         {0, 0, 0, 0, 0, 0},
         {0, 0, 0, 0, 0, 0},
         {0, 0, 0, 0, 0, 0},
         {0, 0, 0, 0, 0, 0},
         {0, 0, 0, 0, 0, 0}},
        {{0, 0, 0, 0, 0, 0},
         {0, 0, 0, 0, 0, 0},
         {0, 0, 0, 0, 0, 0},
         {0, 0, 0, 0, 0, 0},
         {0, 0, 0, 0, 0, 0},
         {0, 0, 0, 0, 0, 0},
         {0, 0, 0, 0, 0, 0},
         {0, 0, 0, 0, 0, 0}},
    },
    {FREQ2FBIN(5180, 0), FREQ2FBIN(5220, 0), FREQ2FBIN(5320, 0), FREQ2FBIN(5400, 0),
     FREQ2FBIN(5500, 0), FREQ2FBIN(5600, 0), FREQ2FBIN(5745, 0),
     FREQ2FBIN(5785, 0)}, /* calTarget_freqbin_5G */
    {FREQ2FBIN(5180, 0), FREQ2FBIN(5240, 0), FREQ2FBIN(5320, 0), FREQ2FBIN(5400, 0),
     FREQ2FBIN(5500, 0), FREQ2FBIN(5700, 0), FREQ2FBIN(5745, 0),
     FREQ2FBIN(5825, 0)}, /* calTarget_freqbin_5GHT20 */
    {FREQ2FBIN(5190, 0), FREQ2FBIN(5230, 0), FREQ2FBIN(5320, 0), FREQ2FBIN(5410, 0),
     FREQ2FBIN(5510, 0), FREQ2FBIN(5670, 0), FREQ2FBIN(5755, 0),
     FREQ2FBIN(5825, 0)}, /* calTarget_freqbin_5GHT40 */
    {
        /* calTargetPower5G */
        {{42, 40, 40, 34}},
        {{42, 40, 40, 34}},
        {{42, 40, 40, 34}},
        {{42, 40, 40, 34}},
        {{42, 40, 40, 34}},
        {{42, 40, 40, 34}},
        {{42, 40, 40, 34}},
        {{42, 40, 40, 34}},
    },
    {
        /* calTargetPower5GHT20 */
        {{40, 40, 40, 40, 32, 28, 40, 40, 32, 28, 40, 40, 32, 20}},
        {{40, 40, 40, 40, 32, 28, 40, 40, 32, 28, 40, 40, 32, 20}},
        {{40, 40, 40, 40, 32, 28, 40, 40, 32, 28, 40, 40, 32, 20}},
        {{40, 40, 40, 40, 32, 28, 40, 40, 32, 28, 40, 40, 32, 20}},
        {{40, 40, 40, 40, 32, 28, 40, 40, 32, 28, 40, 40, 32, 20}},
        {{40, 40, 40, 40, 32, 28, 40, 40, 32, 28, 40, 40, 32, 20}},
        {{38, 38, 38, 38, 32, 28, 38, 38, 32, 28, 38, 38, 32, 26}},
        {{36, 36, 36, 36, 32, 28, 36, 36, 32, 28, 36, 36, 32, 26}},
    },
    {
        /* calTargetPower5GHT40 */
        {{40, 40, 40, 38, 30, 26, 40, 40, 30, 26, 40, 40, 30, 24}},
        {{40, 40, 40, 38, 30, 26, 40, 40, 30, 26, 40, 40, 30, 24}},
        {{40, 40, 40, 38, 30, 26, 40, 40, 30, 26, 40, 40, 30, 24}},
        {{40, 40, 40, 38, 30, 26, 40, 40, 30, 26, 40, 40, 30, 24}},
        {{40, 40, 40, 38, 30, 26, 40, 40, 30, 26, 40, 40, 30, 24}},
        {{40, 40, 40, 38, 30, 26, 40, 40, 30, 26, 40, 40, 30, 24}},
        {{36, 36, 36, 36, 30, 26, 36, 36, 30, 26, 36, 36, 30, 24}},
        {{34, 34, 34, 34, 30, 26, 34, 34, 30, 26, 34, 34, 30, 24}},
    },
    {0x10, 0x16, 0x18, 0x40, 0x46, 0x48, 0x30, 0x36, 0x38}, /* ctlIndex_5G */
    {
        /* ctl_freqbin_5G */
        {FREQ2FBIN(5180, 0), FREQ2FBIN(5260, 0), FREQ2FBIN(5280, 0), FREQ2FBIN(5500, 0),
         FREQ2FBIN(5600, 0), FREQ2FBIN(5700, 0), FREQ2FBIN(5745, 0), FREQ2FBIN(5825, 0)},
        {FREQ2FBIN(5180, 0), FREQ2FBIN(5260, 0), FREQ2FBIN(5280, 0), FREQ2FBIN(5500, 0),
         FREQ2FBIN(5520, 0), FREQ2FBIN(5700, 0), FREQ2FBIN(5745, 0), FREQ2FBIN(5825, 0)},
        {FREQ2FBIN(5190, 0), FREQ2FBIN(5230, 0), FREQ2FBIN(5270, 0), FREQ2FBIN(5310, 0),
         FREQ2FBIN(5510, 0), FREQ2FBIN(5550, 0), FREQ2FBIN(5670, 0), FREQ2FBIN(5755, 0)},
        {FREQ2FBIN(5180, 0), FREQ2FBIN(5200, 0), FREQ2FBIN(5260, 0), FREQ2FBIN(5320, 0),
         FREQ2FBIN(5500, 0), FREQ2FBIN(5700, 0), 0xff, 0xff},
        {FREQ2FBIN(5180, 0), FREQ2FBIN(5260, 0), FREQ2FBIN(5500, 0), FREQ2FBIN(5700, 0), 0xff, 0xff,
         0xff, 0xff},
        {FREQ2FBIN(5190, 0), FREQ2FBIN(5270, 0), FREQ2FBIN(5310, 0), FREQ2FBIN(5510, 0),
         FREQ2FBIN(5590, 0), FREQ2FBIN(5670, 0), 0xff, 0xff},
        {FREQ2FBIN(5180, 0), FREQ2FBIN(5200, 0), FREQ2FBIN(5220, 0), FREQ2FBIN(5260, 0),
         FREQ2FBIN(5500, 0), FREQ2FBIN(5600, 0), FREQ2FBIN(5700, 0), FREQ2FBIN(5745, 0)},
        {FREQ2FBIN(5180, 0), FREQ2FBIN(5260, 0), FREQ2FBIN(5320, 0), FREQ2FBIN(5500, 0),
         FREQ2FBIN(5560, 0), FREQ2FBIN(5700, 0), FREQ2FBIN(5745, 0), FREQ2FBIN(5825, 0)},
        {FREQ2FBIN(5190, 0), FREQ2FBIN(5230, 0), FREQ2FBIN(5270, 0), FREQ2FBIN(5510, 0),
         FREQ2FBIN(5550, 0), FREQ2FBIN(5670, 0), FREQ2FBIN(5755, 0), FREQ2FBIN(5795, 0)},
    },
    {
        /* ctlPowerData_5G */
        {{CTL(60, 1), CTL(60, 1), CTL(60, 1), CTL(60, 1), CTL(60, 1), CTL(60, 1), CTL(60, 1),
          CTL(60, 0)}},
        {{CTL(60, 1), CTL(60, 1), CTL(60, 1), CTL(60, 1), CTL(60, 1), CTL(60, 1), CTL(60, 1),
          CTL(60, 0)}},
        {{CTL(60, 0), CTL(60, 1), CTL(60, 0), CTL(60, 1), CTL(60, 1), CTL(60, 1), CTL(60, 1),
          CTL(60, 1)}},
        {{CTL(60, 0), CTL(60, 1), CTL(60, 1), CTL(60, 0), CTL(60, 1), CTL(60, 0), CTL(60, 0),
          CTL(60, 0)}},
        {{CTL(60, 1), CTL(60, 1), CTL(60, 1), CTL(60, 0), CTL(60, 0), CTL(60, 0), CTL(60, 0),
          CTL(60, 0)}},
        {{CTL(60, 1), CTL(60, 1), CTL(60, 1), CTL(60, 1), CTL(60, 1), CTL(60, 0), CTL(60, 0),
          CTL(60, 0)}},
        {{CTL(60, 1), CTL(60, 1), CTL(60, 1), CTL(60, 1), CTL(60, 1), CTL(60, 1), CTL(60, 1),
          CTL(60, 1)}},
        {{CTL(60, 1), CTL(60, 1), CTL(60, 0), CTL(60, 1), CTL(60, 1), CTL(60, 1), CTL(60, 1),
          CTL(60, 0)}},
        {{CTL(60, 1), CTL(60, 0), CTL(60, 1), CTL(60, 1), CTL(60, 1), CTL(60, 1), CTL(60, 0),
          CTL(60, 1)}},
    },
};

/* In ath9k's order of search. */
static const struct ar9300_eeprom *const ar9300_templates[] = {
    &ar9300_default, &ar9300_x112, &ar9300_h116, &ar9300_h112, &ar9300_x113, NULL};

#endif
