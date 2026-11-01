/*
 * ar9003_initvals.h: the initial register values that more than one chip of
 * the family takes from another chip's table, under the names ath9k gives
 * them: the AR9331 1.1's MAC postamble (ar9330_1p1_initvals.h) and the
 * AR9300 2.2's CCK filter for channel 14 (ar9003_2p2_initvals.h).
 *
 * Copyright (c) 2010-2011 Atheros Communications Inc.
 * Copyright (c) 2011-2012 Qualcomm Atheros Inc.
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

#ifndef _ATH9K_AR9003_INITVALS_H_
#define _ATH9K_AR9003_INITVALS_H_

#include "../../../lib/kint.h"

static const uint32_t ar9331_1p1_mac_postamble[][5] = {
    /* Addr      5G_HT20     5G_HT40     2G_HT40     2G_HT20   */
    {0x00001030, 0x00000230, 0x00000460, 0x000002c0, 0x00000160},
    {0x00001070, 0x00000168, 0x000002d0, 0x00000318, 0x0000018c},
    {0x000010b0, 0x00000e60, 0x00001cc0, 0x00007c70, 0x00003e38},
    {0x00008014, 0x03e803e8, 0x07d007d0, 0x10801600, 0x08400b00},
    {0x0000801c, 0x128d8027, 0x128d804f, 0x12e00057, 0x12e0002b},
    {0x00008120, 0x08f04800, 0x08f04800, 0x08f04810, 0x08f04810},
    {0x000081d0, 0x00003210, 0x00003210, 0x0000320a, 0x0000320a},
    {0x00008318, 0x00003e80, 0x00007d00, 0x00006880, 0x00003440},
};

static const uint32_t ar9300_2p2_baseband_core_txfir_coeff_japan_2484[][2] = {
    /* Addr      allmodes  */
    {0x0000a398, 0x00000000},
    {0x0000a39c, 0x6f7f0301},
    {0x0000a3a0, 0xca9228ee},
};

#endif
