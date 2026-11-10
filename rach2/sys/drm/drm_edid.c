/*
 * drm_edid.c: a sink's EDID, as Linux's drm_edid.c reads it: the checks of
 * its blocks, the connector's EDID blob and size, and the modes each part
 * of the EDID names, in Linux's order and from Linux's tables.
 */

/*
 * The mode tables and the EDID's reading follow Linux 6.12's
 * drivers/gpu/drm/drm_edid.c, under this notice:
 *
 * Copyright (c) 2006 Luc Verhaegen (quirks list)
 * Copyright (c) 2007-2008 Intel Corporation
 *   Jesse Barnes <jesse.barnes@intel.com>
 * Copyright 2010 Red Hat, Inc.
 *
 * DDC probing routines (drm_ddc_read & drm_do_probe_ddc_edid) originally from
 * FB layer.
 *   Copyright (C) 2006 Dennis Munsie <dmunsie@cecropia.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sub license,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * The CVT and GTF formulas follow Linux 6.12's drivers/gpu/drm/drm_modes.c,
 * under this notice:
 *
 * Copyright (c) 1997-2003 by The XFree86 Project, Inc.
 * Copyright (c) 2007 Dave Airlie
 * Copyright (c) 2007-2008 Intel Corporation
 *   Jesse Barnes <jesse.barnes@intel.com>
 * Copyright 2005-2006 Luc Verhaegen
 * Copyright (c) 2001, Andy Ritger  aritger@nvidia.com
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 * Except as contained in this notice, the name of the copyright holder(s)
 * and author(s) shall not be used in advertising or otherwise to promote
 * the sale, use or other dealings in this Software without prior written
 * authorization from the copyright holder(s) and author(s).
 */

#include "drm.h"

#include "../lib/kerrno.h"
#include "../lib/kstring.h"
#include "../mm/kheap.h"

#define EDID_LENGTH 128

/* The display descriptors' types (EDID 1.4, 3.10.3). */
#define EDID_DETAIL_EST_TIMINGS   0xf7
#define EDID_DETAIL_CVT_3BYTE     0xf8
#define EDID_DETAIL_STD_MODES     0xfa
#define EDID_DETAIL_MONITOR_RANGE 0xfd

/* A range descriptor's byte 10: what timings beyond the limits it names. */
#define RANGE_DEFAULT_GTF   0x00
#define RANGE_LIMITS_ONLY   0x01
#define RANGE_SECONDARY_GTF 0x02
#define RANGE_CVT           0x04

/* The extension blocks' tags. */
#define CEA_EXT 0x02
#define VTB_EXT 0x10

/* How standard timings are made when no DMT mode matches, Linux's
 * LEVEL_*. */
#define LEVEL_DMT  0
#define LEVEL_GTF  1
#define LEVEL_GTF2 2
#define LEVEL_CVT  3

/* A detailed timing's flags byte (byte 17). */
#define PT_HSYNC_POSITIVE (1 << 1)
#define PT_VSYNC_POSITIVE (1 << 2)
#define PT_STEREO         (1 << 5)
#define PT_INTERLACED     (1 << 7)

/* A mode of Linux's tables, with the CEA picture aspect in the flags' PIC_AR
 * bits. */
struct edid_mode {
    uint32_t clock; /* kHz */
    uint16_t hdisplay, hsync_start, hsync_end, htotal;
    uint16_t vdisplay, vsync_start, vsync_end, vtotal;
    uint32_t flags; /* DRM_MODE_FLAG_* */
};

#define PH        DRM_MODE_FLAG_PHSYNC
#define NH        DRM_MODE_FLAG_NHSYNC
#define PV        DRM_MODE_FLAG_PVSYNC
#define NV        DRM_MODE_FLAG_NVSYNC
#define IL        DRM_MODE_FLAG_INTERLACE
#define DC        DRM_MODE_FLAG_DBLCLK
#define DS        DRM_MODE_FLAG_DBLSCAN
#define AR4_3     DRM_MODE_FLAG_PIC_AR_4_3
#define AR16_9    DRM_MODE_FLAG_PIC_AR_16_9
#define AR64_27   DRM_MODE_FLAG_PIC_AR_64_27
#define AR256_135 DRM_MODE_FLAG_PIC_AR_256_135

/*
 * The tables are Linux 6.12's drm_dmt_modes, edid_est_modes,
 * edid_cea_modes_1, edid_cea_modes_193 and edid_4k_modes, in their order:
 * the VESA DMT modes, the established timings by bit, the CEA modes by VIC
 * from 1 and from 193, and the HDMI 1.4 modes by HDMI VIC from 0 (unused).
 */
static const struct edid_mode dmt_modes[] = {
    {31500, 640, 672, 736, 832, 350, 382, 385, 445, PH | NV},
    {31500, 640, 672, 736, 832, 400, 401, 404, 445, NH | PV},
    {35500, 720, 756, 828, 936, 400, 401, 404, 446, NH | PV},
    {25175, 640, 656, 752, 800, 480, 490, 492, 525, NH | NV},
    {31500, 640, 664, 704, 832, 480, 489, 492, 520, NH | NV},
    {31500, 640, 656, 720, 840, 480, 481, 484, 500, NH | NV},
    {36000, 640, 696, 752, 832, 480, 481, 484, 509, NH | NV},
    {36000, 800, 824, 896, 1024, 600, 601, 603, 625, PH | PV},
    {40000, 800, 840, 968, 1056, 600, 601, 605, 628, PH | PV},
    {50000, 800, 856, 976, 1040, 600, 637, 643, 666, PH | PV},
    {49500, 800, 816, 896, 1056, 600, 601, 604, 625, PH | PV},
    {56250, 800, 832, 896, 1048, 600, 601, 604, 631, PH | PV},
    {73250, 800, 848, 880, 960, 600, 603, 607, 636, PH | NV},
    {33750, 848, 864, 976, 1088, 480, 486, 494, 517, PH | PV},
    {44900, 1024, 1032, 1208, 1264, 768, 768, 776, 817, PH | PV | IL},
    {65000, 1024, 1048, 1184, 1344, 768, 771, 777, 806, NH | NV},
    {75000, 1024, 1048, 1184, 1328, 768, 771, 777, 806, NH | NV},
    {78750, 1024, 1040, 1136, 1312, 768, 769, 772, 800, PH | PV},
    {94500, 1024, 1072, 1168, 1376, 768, 769, 772, 808, PH | PV},
    {115500, 1024, 1072, 1104, 1184, 768, 771, 775, 813, PH | NV},
    {108000, 1152, 1216, 1344, 1600, 864, 865, 868, 900, PH | PV},
    {74250, 1280, 1390, 1430, 1650, 720, 725, 730, 750, PH | PV},
    {68250, 1280, 1328, 1360, 1440, 768, 771, 778, 790, PH | NV},
    {79500, 1280, 1344, 1472, 1664, 768, 771, 778, 798, NH | PV},
    {102250, 1280, 1360, 1488, 1696, 768, 771, 778, 805, NH | PV},
    {117500, 1280, 1360, 1496, 1712, 768, 771, 778, 809, NH | PV},
    {140250, 1280, 1328, 1360, 1440, 768, 771, 778, 813, PH | NV},
    {71000, 1280, 1328, 1360, 1440, 800, 803, 809, 823, PH | NV},
    {83500, 1280, 1352, 1480, 1680, 800, 803, 809, 831, NH | PV},
    {106500, 1280, 1360, 1488, 1696, 800, 803, 809, 838, NH | PV},
    {122500, 1280, 1360, 1496, 1712, 800, 803, 809, 843, NH | PV},
    {146250, 1280, 1328, 1360, 1440, 800, 803, 809, 847, PH | NV},
    {108000, 1280, 1376, 1488, 1800, 960, 961, 964, 1000, PH | PV},
    {148500, 1280, 1344, 1504, 1728, 960, 961, 964, 1011, PH | PV},
    {175500, 1280, 1328, 1360, 1440, 960, 963, 967, 1017, PH | NV},
    {108000, 1280, 1328, 1440, 1688, 1024, 1025, 1028, 1066, PH | PV},
    {135000, 1280, 1296, 1440, 1688, 1024, 1025, 1028, 1066, PH | PV},
    {157500, 1280, 1344, 1504, 1728, 1024, 1025, 1028, 1072, PH | PV},
    {187250, 1280, 1328, 1360, 1440, 1024, 1027, 1034, 1084, PH | NV},
    {85500, 1360, 1424, 1536, 1792, 768, 771, 777, 795, PH | PV},
    {148250, 1360, 1408, 1440, 1520, 768, 771, 776, 813, PH | NV},
    {85500, 1366, 1436, 1579, 1792, 768, 771, 774, 798, PH | PV},
    {72000, 1366, 1380, 1436, 1500, 768, 769, 772, 800, PH | PV},
    {101000, 1400, 1448, 1480, 1560, 1050, 1053, 1057, 1080, PH | NV},
    {121750, 1400, 1488, 1632, 1864, 1050, 1053, 1057, 1089, NH | PV},
    {156000, 1400, 1504, 1648, 1896, 1050, 1053, 1057, 1099, NH | PV},
    {179500, 1400, 1504, 1656, 1912, 1050, 1053, 1057, 1105, NH | PV},
    {208000, 1400, 1448, 1480, 1560, 1050, 1053, 1057, 1112, PH | NV},
    {88750, 1440, 1488, 1520, 1600, 900, 903, 909, 926, PH | NV},
    {106500, 1440, 1520, 1672, 1904, 900, 903, 909, 934, NH | PV},
    {136750, 1440, 1536, 1688, 1936, 900, 903, 909, 942, NH | PV},
    {157000, 1440, 1544, 1696, 1952, 900, 903, 909, 948, NH | PV},
    {182750, 1440, 1488, 1520, 1600, 900, 903, 909, 953, PH | NV},
    {108000, 1600, 1624, 1704, 1800, 900, 901, 904, 1000, PH | PV},
    {162000, 1600, 1664, 1856, 2160, 1200, 1201, 1204, 1250, PH | PV},
    {175500, 1600, 1664, 1856, 2160, 1200, 1201, 1204, 1250, PH | PV},
    {189000, 1600, 1664, 1856, 2160, 1200, 1201, 1204, 1250, PH | PV},
    {202500, 1600, 1664, 1856, 2160, 1200, 1201, 1204, 1250, PH | PV},
    {229500, 1600, 1664, 1856, 2160, 1200, 1201, 1204, 1250, PH | PV},
    {268250, 1600, 1648, 1680, 1760, 1200, 1203, 1207, 1271, PH | NV},
    {119000, 1680, 1728, 1760, 1840, 1050, 1053, 1059, 1080, PH | NV},
    {146250, 1680, 1784, 1960, 2240, 1050, 1053, 1059, 1089, NH | PV},
    {187000, 1680, 1800, 1976, 2272, 1050, 1053, 1059, 1099, NH | PV},
    {214750, 1680, 1808, 1984, 2288, 1050, 1053, 1059, 1105, NH | PV},
    {245500, 1680, 1728, 1760, 1840, 1050, 1053, 1059, 1112, PH | NV},
    {204750, 1792, 1920, 2120, 2448, 1344, 1345, 1348, 1394, NH | PV},
    {261000, 1792, 1888, 2104, 2456, 1344, 1345, 1348, 1417, NH | PV},
    {333250, 1792, 1840, 1872, 1952, 1344, 1347, 1351, 1423, PH | NV},
    {218250, 1856, 1952, 2176, 2528, 1392, 1393, 1396, 1439, NH | PV},
    {288000, 1856, 1984, 2208, 2560, 1392, 1393, 1396, 1500, NH | PV},
    {356500, 1856, 1904, 1936, 2016, 1392, 1395, 1399, 1474, PH | NV},
    {148500, 1920, 2008, 2052, 2200, 1080, 1084, 1089, 1125, NH | NV},
    {154000, 1920, 1968, 2000, 2080, 1200, 1203, 1209, 1235, PH | NV},
    {193250, 1920, 2056, 2256, 2592, 1200, 1203, 1209, 1245, NH | PV},
    {245250, 1920, 2056, 2264, 2608, 1200, 1203, 1209, 1255, NH | PV},
    {281250, 1920, 2064, 2272, 2624, 1200, 1203, 1209, 1262, NH | PV},
    {317000, 1920, 1968, 2000, 2080, 1200, 1203, 1209, 1271, PH | NV},
    {234000, 1920, 2048, 2256, 2600, 1440, 1441, 1444, 1500, NH | PV},
    {297000, 1920, 2064, 2288, 2640, 1440, 1441, 1444, 1500, NH | PV},
    {380500, 1920, 1968, 2000, 2080, 1440, 1443, 1447, 1525, PH | NV},
    {162000, 2048, 2074, 2154, 2250, 1152, 1153, 1156, 1200, PH | PV},
    {268500, 2560, 2608, 2640, 2720, 1600, 1603, 1609, 1646, PH | NV},
    {348500, 2560, 2752, 3032, 3504, 1600, 1603, 1609, 1658, NH | PV},
    {443250, 2560, 2768, 3048, 3536, 1600, 1603, 1609, 1672, NH | PV},
    {505250, 2560, 2768, 3048, 3536, 1600, 1603, 1609, 1682, NH | PV},
    {552750, 2560, 2608, 2640, 2720, 1600, 1603, 1609, 1694, PH | NV},
    {556744, 4096, 4104, 4136, 4176, 2160, 2208, 2216, 2222, PH | NV},
    {556188, 4096, 4104, 4136, 4176, 2160, 2208, 2216, 2222, PH | NV},
};

static const struct edid_mode est_modes[] = {
    {40000, 800, 840, 968, 1056, 600, 601, 605, 628, PH | PV},
    {36000, 800, 824, 896, 1024, 600, 601, 603, 625, PH | PV},
    {31500, 640, 656, 720, 840, 480, 481, 484, 500, NH | NV},
    {31500, 640, 664, 704, 832, 480, 489, 492, 520, NH | NV},
    {30240, 640, 704, 768, 864, 480, 483, 486, 525, NH | NV},
    {25175, 640, 656, 752, 800, 480, 490, 492, 525, NH | NV},
    {35500, 720, 738, 846, 900, 400, 421, 423, 449, NH | NV},
    {28320, 720, 738, 846, 900, 400, 412, 414, 449, NH | PV},
    {135000, 1280, 1296, 1440, 1688, 1024, 1025, 1028, 1066, PH | PV},
    {78750, 1024, 1040, 1136, 1312, 768, 769, 772, 800, PH | PV},
    {75000, 1024, 1048, 1184, 1328, 768, 771, 777, 806, NH | NV},
    {65000, 1024, 1048, 1184, 1344, 768, 771, 777, 806, NH | NV},
    {44900, 1024, 1032, 1208, 1264, 768, 768, 776, 817, PH | PV | IL},
    {57284, 832, 864, 928, 1152, 624, 625, 628, 667, NH | NV},
    {49500, 800, 816, 896, 1056, 600, 601, 604, 625, PH | PV},
    {50000, 800, 856, 976, 1040, 600, 637, 643, 666, PH | PV},
    {108000, 1152, 1216, 1344, 1600, 864, 865, 868, 900, PH | PV},
};

static const struct edid_mode cea_modes_1[] = {
    {25175, 640, 656, 752, 800, 480, 490, 492, 525, NH | NV | AR4_3},
    {27000, 720, 736, 798, 858, 480, 489, 495, 525, NH | NV | AR4_3},
    {27000, 720, 736, 798, 858, 480, 489, 495, 525, NH | NV | AR16_9},
    {74250, 1280, 1390, 1430, 1650, 720, 725, 730, 750, PH | PV | AR16_9},
    {74250, 1920, 2008, 2052, 2200, 1080, 1084, 1094, 1125, PH | PV | IL | AR16_9},
    {13500, 720, 739, 801, 858, 480, 488, 494, 525, NH | NV | IL | DC | AR4_3},
    {13500, 720, 739, 801, 858, 480, 488, 494, 525, NH | NV | IL | DC | AR16_9},
    {13500, 720, 739, 801, 858, 240, 244, 247, 262, NH | NV | DC | AR4_3},
    {13500, 720, 739, 801, 858, 240, 244, 247, 262, NH | NV | DC | AR16_9},
    {54000, 2880, 2956, 3204, 3432, 480, 488, 494, 525, NH | NV | IL | AR4_3},
    {54000, 2880, 2956, 3204, 3432, 480, 488, 494, 525, NH | NV | IL | AR16_9},
    {54000, 2880, 2956, 3204, 3432, 240, 244, 247, 262, NH | NV | AR4_3},
    {54000, 2880, 2956, 3204, 3432, 240, 244, 247, 262, NH | NV | AR16_9},
    {54000, 1440, 1472, 1596, 1716, 480, 489, 495, 525, NH | NV | AR4_3},
    {54000, 1440, 1472, 1596, 1716, 480, 489, 495, 525, NH | NV | AR16_9},
    {148500, 1920, 2008, 2052, 2200, 1080, 1084, 1089, 1125, PH | PV | AR16_9},
    {27000, 720, 732, 796, 864, 576, 581, 586, 625, NH | NV | AR4_3},
    {27000, 720, 732, 796, 864, 576, 581, 586, 625, NH | NV | AR16_9},
    {74250, 1280, 1720, 1760, 1980, 720, 725, 730, 750, PH | PV | AR16_9},
    {74250, 1920, 2448, 2492, 2640, 1080, 1084, 1094, 1125, PH | PV | IL | AR16_9},
    {13500, 720, 732, 795, 864, 576, 580, 586, 625, NH | NV | IL | DC | AR4_3},
    {13500, 720, 732, 795, 864, 576, 580, 586, 625, NH | NV | IL | DC | AR16_9},
    {13500, 720, 732, 795, 864, 288, 290, 293, 312, NH | NV | DC | AR4_3},
    {13500, 720, 732, 795, 864, 288, 290, 293, 312, NH | NV | DC | AR16_9},
    {54000, 2880, 2928, 3180, 3456, 576, 580, 586, 625, NH | NV | IL | AR4_3},
    {54000, 2880, 2928, 3180, 3456, 576, 580, 586, 625, NH | NV | IL | AR16_9},
    {54000, 2880, 2928, 3180, 3456, 288, 290, 293, 312, NH | NV | AR4_3},
    {54000, 2880, 2928, 3180, 3456, 288, 290, 293, 312, NH | NV | AR16_9},
    {54000, 1440, 1464, 1592, 1728, 576, 581, 586, 625, NH | NV | AR4_3},
    {54000, 1440, 1464, 1592, 1728, 576, 581, 586, 625, NH | NV | AR16_9},
    {148500, 1920, 2448, 2492, 2640, 1080, 1084, 1089, 1125, PH | PV | AR16_9},
    {74250, 1920, 2558, 2602, 2750, 1080, 1084, 1089, 1125, PH | PV | AR16_9},
    {74250, 1920, 2448, 2492, 2640, 1080, 1084, 1089, 1125, PH | PV | AR16_9},
    {74250, 1920, 2008, 2052, 2200, 1080, 1084, 1089, 1125, PH | PV | AR16_9},
    {108000, 2880, 2944, 3192, 3432, 480, 489, 495, 525, NH | NV | AR4_3},
    {108000, 2880, 2944, 3192, 3432, 480, 489, 495, 525, NH | NV | AR16_9},
    {108000, 2880, 2928, 3184, 3456, 576, 581, 586, 625, NH | NV | AR4_3},
    {108000, 2880, 2928, 3184, 3456, 576, 581, 586, 625, NH | NV | AR16_9},
    {72000, 1920, 1952, 2120, 2304, 1080, 1126, 1136, 1250, PH | NV | IL | AR16_9},
    {148500, 1920, 2448, 2492, 2640, 1080, 1084, 1094, 1125, PH | PV | IL | AR16_9},
    {148500, 1280, 1720, 1760, 1980, 720, 725, 730, 750, PH | PV | AR16_9},
    {54000, 720, 732, 796, 864, 576, 581, 586, 625, NH | NV | AR4_3},
    {54000, 720, 732, 796, 864, 576, 581, 586, 625, NH | NV | AR16_9},
    {27000, 720, 732, 795, 864, 576, 580, 586, 625, NH | NV | IL | DC | AR4_3},
    {27000, 720, 732, 795, 864, 576, 580, 586, 625, NH | NV | IL | DC | AR16_9},
    {148500, 1920, 2008, 2052, 2200, 1080, 1084, 1094, 1125, PH | PV | IL | AR16_9},
    {148500, 1280, 1390, 1430, 1650, 720, 725, 730, 750, PH | PV | AR16_9},
    {54000, 720, 736, 798, 858, 480, 489, 495, 525, NH | NV | AR4_3},
    {54000, 720, 736, 798, 858, 480, 489, 495, 525, NH | NV | AR16_9},
    {27000, 720, 739, 801, 858, 480, 488, 494, 525, NH | NV | IL | DC | AR4_3},
    {27000, 720, 739, 801, 858, 480, 488, 494, 525, NH | NV | IL | DC | AR16_9},
    {108000, 720, 732, 796, 864, 576, 581, 586, 625, NH | NV | AR4_3},
    {108000, 720, 732, 796, 864, 576, 581, 586, 625, NH | NV | AR16_9},
    {54000, 720, 732, 795, 864, 576, 580, 586, 625, NH | NV | IL | DC | AR4_3},
    {54000, 720, 732, 795, 864, 576, 580, 586, 625, NH | NV | IL | DC | AR16_9},
    {108000, 720, 736, 798, 858, 480, 489, 495, 525, NH | NV | AR4_3},
    {108000, 720, 736, 798, 858, 480, 489, 495, 525, NH | NV | AR16_9},
    {54000, 720, 739, 801, 858, 480, 488, 494, 525, NH | NV | IL | DC | AR4_3},
    {54000, 720, 739, 801, 858, 480, 488, 494, 525, NH | NV | IL | DC | AR16_9},
    {59400, 1280, 3040, 3080, 3300, 720, 725, 730, 750, PH | PV | AR16_9},
    {74250, 1280, 3700, 3740, 3960, 720, 725, 730, 750, PH | PV | AR16_9},
    {74250, 1280, 3040, 3080, 3300, 720, 725, 730, 750, PH | PV | AR16_9},
    {297000, 1920, 2008, 2052, 2200, 1080, 1084, 1089, 1125, PH | PV | AR16_9},
    {297000, 1920, 2448, 2492, 2640, 1080, 1084, 1089, 1125, PH | PV | AR16_9},
    {59400, 1280, 3040, 3080, 3300, 720, 725, 730, 750, PH | PV | AR64_27},
    {74250, 1280, 3700, 3740, 3960, 720, 725, 730, 750, PH | PV | AR64_27},
    {74250, 1280, 3040, 3080, 3300, 720, 725, 730, 750, PH | PV | AR64_27},
    {74250, 1280, 1720, 1760, 1980, 720, 725, 730, 750, PH | PV | AR64_27},
    {74250, 1280, 1390, 1430, 1650, 720, 725, 730, 750, PH | PV | AR64_27},
    {148500, 1280, 1720, 1760, 1980, 720, 725, 730, 750, PH | PV | AR64_27},
    {148500, 1280, 1390, 1430, 1650, 720, 725, 730, 750, PH | PV | AR64_27},
    {74250, 1920, 2558, 2602, 2750, 1080, 1084, 1089, 1125, PH | PV | AR64_27},
    {74250, 1920, 2448, 2492, 2640, 1080, 1084, 1089, 1125, PH | PV | AR64_27},
    {74250, 1920, 2008, 2052, 2200, 1080, 1084, 1089, 1125, PH | PV | AR64_27},
    {148500, 1920, 2448, 2492, 2640, 1080, 1084, 1089, 1125, PH | PV | AR64_27},
    {148500, 1920, 2008, 2052, 2200, 1080, 1084, 1089, 1125, PH | PV | AR64_27},
    {297000, 1920, 2448, 2492, 2640, 1080, 1084, 1089, 1125, PH | PV | AR64_27},
    {297000, 1920, 2008, 2052, 2200, 1080, 1084, 1089, 1125, PH | PV | AR64_27},
    {59400, 1680, 3040, 3080, 3300, 720, 725, 730, 750, PH | PV | AR64_27},
    {59400, 1680, 2908, 2948, 3168, 720, 725, 730, 750, PH | PV | AR64_27},
    {59400, 1680, 2380, 2420, 2640, 720, 725, 730, 750, PH | PV | AR64_27},
    {82500, 1680, 1940, 1980, 2200, 720, 725, 730, 750, PH | PV | AR64_27},
    {99000, 1680, 1940, 1980, 2200, 720, 725, 730, 750, PH | PV | AR64_27},
    {165000, 1680, 1740, 1780, 2000, 720, 725, 730, 825, PH | PV | AR64_27},
    {198000, 1680, 1740, 1780, 2000, 720, 725, 730, 825, PH | PV | AR64_27},
    {99000, 2560, 3558, 3602, 3750, 1080, 1084, 1089, 1100, PH | PV | AR64_27},
    {90000, 2560, 3008, 3052, 3200, 1080, 1084, 1089, 1125, PH | PV | AR64_27},
    {118800, 2560, 3328, 3372, 3520, 1080, 1084, 1089, 1125, PH | PV | AR64_27},
    {185625, 2560, 3108, 3152, 3300, 1080, 1084, 1089, 1125, PH | PV | AR64_27},
    {198000, 2560, 2808, 2852, 3000, 1080, 1084, 1089, 1100, PH | PV | AR64_27},
    {371250, 2560, 2778, 2822, 2970, 1080, 1084, 1089, 1250, PH | PV | AR64_27},
    {495000, 2560, 3108, 3152, 3300, 1080, 1084, 1089, 1250, PH | PV | AR64_27},
    {297000, 3840, 5116, 5204, 5500, 2160, 2168, 2178, 2250, PH | PV | AR16_9},
    {297000, 3840, 4896, 4984, 5280, 2160, 2168, 2178, 2250, PH | PV | AR16_9},
    {297000, 3840, 4016, 4104, 4400, 2160, 2168, 2178, 2250, PH | PV | AR16_9},
    {594000, 3840, 4896, 4984, 5280, 2160, 2168, 2178, 2250, PH | PV | AR16_9},
    {594000, 3840, 4016, 4104, 4400, 2160, 2168, 2178, 2250, PH | PV | AR16_9},
    {297000, 4096, 5116, 5204, 5500, 2160, 2168, 2178, 2250, PH | PV | AR256_135},
    {297000, 4096, 5064, 5152, 5280, 2160, 2168, 2178, 2250, PH | PV | AR256_135},
    {297000, 4096, 4184, 4272, 4400, 2160, 2168, 2178, 2250, PH | PV | AR256_135},
    {594000, 4096, 5064, 5152, 5280, 2160, 2168, 2178, 2250, PH | PV | AR256_135},
    {594000, 4096, 4184, 4272, 4400, 2160, 2168, 2178, 2250, PH | PV | AR256_135},
    {297000, 3840, 5116, 5204, 5500, 2160, 2168, 2178, 2250, PH | PV | AR64_27},
    {297000, 3840, 4896, 4984, 5280, 2160, 2168, 2178, 2250, PH | PV | AR64_27},
    {297000, 3840, 4016, 4104, 4400, 2160, 2168, 2178, 2250, PH | PV | AR64_27},
    {594000, 3840, 4896, 4984, 5280, 2160, 2168, 2178, 2250, PH | PV | AR64_27},
    {594000, 3840, 4016, 4104, 4400, 2160, 2168, 2178, 2250, PH | PV | AR64_27},
    {90000, 1280, 2240, 2280, 2500, 720, 725, 730, 750, PH | PV | AR16_9},
    {90000, 1280, 2240, 2280, 2500, 720, 725, 730, 750, PH | PV | AR64_27},
    {99000, 1680, 2490, 2530, 2750, 720, 725, 730, 750, PH | PV | AR64_27},
    {148500, 1920, 2558, 2602, 2750, 1080, 1084, 1089, 1125, PH | PV | AR16_9},
    {148500, 1920, 2558, 2602, 2750, 1080, 1084, 1089, 1125, PH | PV | AR64_27},
    {198000, 2560, 3558, 3602, 3750, 1080, 1084, 1089, 1100, PH | PV | AR64_27},
    {594000, 3840, 5116, 5204, 5500, 2160, 2168, 2178, 2250, PH | PV | AR16_9},
    {594000, 4096, 5116, 5204, 5500, 2160, 2168, 2178, 2250, PH | PV | AR256_135},
    {594000, 3840, 5116, 5204, 5500, 2160, 2168, 2178, 2250, PH | PV | AR64_27},
    {1188000, 3840, 4896, 4984, 5280, 2160, 2168, 2178, 2250, PH | PV | AR16_9},
    {1188000, 3840, 4016, 4104, 4400, 2160, 2168, 2178, 2250, PH | PV | AR16_9},
    {1188000, 3840, 4896, 4984, 5280, 2160, 2168, 2178, 2250, PH | PV | AR64_27},
    {1188000, 3840, 4016, 4104, 4400, 2160, 2168, 2178, 2250, PH | PV | AR64_27},
    {396000, 5120, 7116, 7204, 7500, 2160, 2168, 2178, 2200, PH | PV | AR64_27},
    {396000, 5120, 6816, 6904, 7200, 2160, 2168, 2178, 2200, PH | PV | AR64_27},
    {396000, 5120, 5784, 5872, 6000, 2160, 2168, 2178, 2200, PH | PV | AR64_27},
    {742500, 5120, 5866, 5954, 6250, 2160, 2168, 2178, 2475, PH | PV | AR64_27},
    {742500, 5120, 6216, 6304, 6600, 2160, 2168, 2178, 2250, PH | PV | AR64_27},
    {742500, 5120, 5284, 5372, 5500, 2160, 2168, 2178, 2250, PH | PV | AR64_27},
    {1485000, 5120, 6216, 6304, 6600, 2160, 2168, 2178, 2250, PH | PV | AR64_27},
};

static const struct edid_mode cea_modes_193[] = {
    {1485000, 5120, 5284, 5372, 5500, 2160, 2168, 2178, 2250, PH | PV | AR64_27},
    {1188000, 7680, 10232, 10408, 11000, 4320, 4336, 4356, 4500, PH | PV | AR16_9},
    {1188000, 7680, 10032, 10208, 10800, 4320, 4336, 4356, 4400, PH | PV | AR16_9},
    {1188000, 7680, 8232, 8408, 9000, 4320, 4336, 4356, 4400, PH | PV | AR16_9},
    {2376000, 7680, 10232, 10408, 11000, 4320, 4336, 4356, 4500, PH | PV | AR16_9},
    {2376000, 7680, 10032, 10208, 10800, 4320, 4336, 4356, 4400, PH | PV | AR16_9},
    {2376000, 7680, 8232, 8408, 9000, 4320, 4336, 4356, 4400, PH | PV | AR16_9},
    {4752000, 7680, 9792, 9968, 10560, 4320, 4336, 4356, 4500, PH | PV | AR16_9},
    {4752000, 7680, 8032, 8208, 8800, 4320, 4336, 4356, 4500, PH | PV | AR16_9},
    {1188000, 7680, 10232, 10408, 11000, 4320, 4336, 4356, 4500, PH | PV | AR64_27},
    {1188000, 7680, 10032, 10208, 10800, 4320, 4336, 4356, 4400, PH | PV | AR64_27},
    {1188000, 7680, 8232, 8408, 9000, 4320, 4336, 4356, 4400, PH | PV | AR64_27},
    {2376000, 7680, 10232, 10408, 11000, 4320, 4336, 4356, 4500, PH | PV | AR64_27},
    {2376000, 7680, 10032, 10208, 10800, 4320, 4336, 4356, 4400, PH | PV | AR64_27},
    {2376000, 7680, 8232, 8408, 9000, 4320, 4336, 4356, 4400, PH | PV | AR64_27},
    {4752000, 7680, 9792, 9968, 10560, 4320, 4336, 4356, 4500, PH | PV | AR64_27},
    {4752000, 7680, 8032, 8208, 8800, 4320, 4336, 4356, 4500, PH | PV | AR64_27},
    {1485000, 10240, 11732, 11908, 12500, 4320, 4336, 4356, 4950, PH | PV | AR64_27},
    {1485000, 10240, 12732, 12908, 13500, 4320, 4336, 4356, 4400, PH | PV | AR64_27},
    {1485000, 10240, 10528, 10704, 11000, 4320, 4336, 4356, 4500, PH | PV | AR64_27},
    {2970000, 10240, 11732, 11908, 12500, 4320, 4336, 4356, 4950, PH | PV | AR64_27},
    {2970000, 10240, 12732, 12908, 13500, 4320, 4336, 4356, 4400, PH | PV | AR64_27},
    {2970000, 10240, 10528, 10704, 11000, 4320, 4336, 4356, 4500, PH | PV | AR64_27},
    {5940000, 10240, 12432, 12608, 13200, 4320, 4336, 4356, 4500, PH | PV | AR64_27},
    {5940000, 10240, 10528, 10704, 11000, 4320, 4336, 4356, 4500, PH | PV | AR64_27},
    {1188000, 4096, 4896, 4984, 5280, 2160, 2168, 2178, 2250, PH | PV | AR256_135},
    {1188000, 4096, 4184, 4272, 4400, 2160, 2168, 2178, 2250, PH | PV | AR256_135},
};

static const struct edid_mode hdmi_modes[] = {
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {297000, 3840, 4016, 4104, 4400, 2160, 2168, 2178, 2250, PH | PV | AR16_9},
    {297000, 3840, 4896, 4984, 5280, 2160, 2168, 2178, 2250, PH | PV | AR16_9},
    {297000, 3840, 5116, 5204, 5500, 2160, 2168, 2178, 2250, PH | PV | AR16_9},
    {297000, 4096, 5116, 5204, 5500, 2160, 2168, 2178, 2250, PH | PV | AR256_135},
};

/* A mode named by size and refresh, for the established timings III and
 * the inferred modes; `rb` asks for reduced blanking. */
struct minimode {
    uint16_t w, h;
    uint8_t r, rb;
};

/* Linux's est3_modes, by bit from byte 6, bit 7, of the descriptor. */
static const struct minimode est3_modes[] = {
    {640, 350, 85, 0},   {640, 400, 85, 0},   {720, 400, 85, 0},   {640, 480, 85, 0},
    {848, 480, 60, 0},   {800, 600, 85, 0},   {1024, 768, 85, 0},  {1152, 864, 75, 0},
    {1280, 768, 60, 1},  {1280, 768, 60, 0},  {1280, 768, 75, 0},  {1280, 768, 85, 0},
    {1280, 960, 60, 0},  {1280, 960, 85, 0},  {1280, 1024, 60, 0}, {1280, 1024, 85, 0},
    {1360, 768, 60, 0},  {1440, 900, 60, 1},  {1440, 900, 60, 0},  {1440, 900, 75, 0},
    {1440, 900, 85, 0},  {1400, 1050, 60, 1}, {1400, 1050, 60, 0}, {1400, 1050, 75, 0},
    {1400, 1050, 85, 0}, {1680, 1050, 60, 1}, {1680, 1050, 60, 0}, {1680, 1050, 75, 0},
    {1680, 1050, 85, 0}, {1600, 1200, 60, 0}, {1600, 1200, 65, 0}, {1600, 1200, 70, 0},
    {1600, 1200, 75, 0}, {1600, 1200, 85, 0}, {1792, 1344, 60, 0}, {1792, 1344, 75, 0},
    {1856, 1392, 60, 0}, {1856, 1392, 75, 0}, {1920, 1200, 60, 1}, {1920, 1200, 60, 0},
    {1920, 1200, 75, 0}, {1920, 1200, 85, 0}, {1920, 1440, 60, 0}, {1920, 1440, 75, 0},
};

/* Linux's extra_modes, the sizes a range descriptor's formula is tried at. */
static const struct minimode extra_modes[] = {
    {1024, 576, 60, 0},  {1366, 768, 60, 0},  {1600, 900, 60, 0},  {1680, 945, 60, 0},
    {1920, 1080, 60, 0}, {2048, 1152, 60, 0}, {2048, 1536, 60, 0},
};

#define NELEM(a) ((int)(sizeof(a) / sizeof((a)[0])))

/* ------------------------------------------------------------------------
 * Blocks
 */

static const uint8_t edid_header[8] = {0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00};

/** How many of the base block's eight header bytes are right. */
static int header_score(const uint8_t *block)
{
    int i, score = 0;

    for (i = 0; i < 8; i++) {
        score += block[i] == edid_header[i];
    }
    return score;
}

/** The checksum byte `block` should have, Linux's
 * edid_block_compute_checksum. */
static uint8_t block_checksum(const uint8_t *block)
{
    uint8_t sum = 0;
    int i;

    for (i = 0; i < EDID_LENGTH - 1; i++) {
        sum = (uint8_t)(sum + block[i]);
    }
    return (uint8_t)(0x100 - sum);
}

/** Whether a block is valid as Linux's edid_block_check and
 * edid_block_status_valid judge it: a base block with a header of at least
 * six right bytes (repaired in place), its checksum and version 1; an
 * extension with its checksum, or a CTA extension whatever its checksum. */
static int block_valid(uint8_t *block, int base)
{
    int score;

    if (base) {
        score = header_score(block);
        if (score < 6) {
            return 0;
        }
        if (score < 8) {
            k_memcpy(block, edid_header, sizeof(edid_header));
        }
    }
    if (block_checksum(block) != block[EDID_LENGTH - 1] && (base || block[0] != CEA_EXT)) {
        return 0;
    }
    return !base || block[0x12] == 1;
}

/**
 * Copy the EDID `edid` of `len` bytes as Linux keeps one it read: the base
 * block repaired, the extensions it names that are there and valid, and the
 * base's count and checksum made to match. Returns the copy, kmalloc'd, with
 * its block count in `*blocks`, or NULL for an EDID whose base block is not
 * valid or with no memory.
 */
static uint8_t *edid_clean(const uint8_t *edid, uint32_t len, uint32_t *blocks)
{
    uint32_t want, have, i, n = 1;
    uint8_t *copy;

    if (!edid || len < EDID_LENGTH) {
        return NULL;
    }
    want = 1u + edid[0x7e];
    have = len / EDID_LENGTH;
    copy = (uint8_t *)kmalloc((want < have ? want : have) * EDID_LENGTH);
    if (!copy) {
        return NULL;
    }
    k_memcpy(copy, edid, EDID_LENGTH);
    if (!block_valid(copy, 1)) {
        kfree(copy);
        return NULL;
    }
    for (i = 1; i < want && i < have; i++) {
        k_memcpy(copy + n * EDID_LENGTH, edid + i * EDID_LENGTH, EDID_LENGTH);
        n += (uint32_t)block_valid(copy + n * EDID_LENGTH, 0);
    }
    if (n != want) {
        copy[0x7e] = (uint8_t)(n - 1);
        copy[EDID_LENGTH - 1] = block_checksum(copy);
    }
    *blocks = n;
    return copy;
}

/* ------------------------------------------------------------------------
 * The parse
 */

/*
 * A parse of one cleaned EDID: its blocks, and the modes found so far,
 * which play the part of Linux's connector->probed_modes, since some steps
 * look at what earlier ones added. `vics` are the VICs of the CTA video
 * data blocks, 0 for one the tables lack; `hdmi` is what a walk for
 * drm_edid_hdmi_info fills.
 */
struct edid_parse {
    const uint8_t *edid;
    uint32_t blocks;
    struct drm_display_mode *head, **tail;
    int count;
    int nomem;
    uint8_t vics[256];
    uint32_t nvics;
    struct drm_edid_hdmi_info *hdmi;
};

/** The base block's revision, EDID 1.x's x. */
#define REVISION(p) ((p)->edid[0x13])

/** Append a copy of `m` to the modes found. */
static void parse_add(struct edid_parse *p, const struct drm_mode_modeinfo *m)
{
    struct drm_display_mode *d = (struct drm_display_mode *)kmalloc(sizeof(*d));

    if (!d) {
        p->nomem = 1;
        return;
    }
    d->info = *m;
    d->next = NULL;
    *p->tail = d;
    p->tail = &d->next;
    p->count++;
}

/** Make `m` the mode `t` of a table, of type DRIVER, named. */
static void mode_from_table(struct drm_mode_modeinfo *m, const struct edid_mode *t)
{
    k_memset(m, 0, sizeof(*m));
    m->clock = t->clock;
    m->hdisplay = t->hdisplay;
    m->hsync_start = t->hsync_start;
    m->hsync_end = t->hsync_end;
    m->htotal = t->htotal;
    m->vdisplay = t->vdisplay;
    m->vsync_start = t->vsync_start;
    m->vsync_end = t->vsync_end;
    m->vtotal = t->vtotal;
    m->flags = t->flags;
    m->type = DRM_MODE_TYPE_DRIVER;
    drm_mode_set_name(m);
}

/** Linux's mode_is_rb: whether a mode has CVT's reduced blanking. */
static int mode_is_rb(const struct drm_mode_modeinfo *m)
{
    return m->htotal - m->hdisplay == 160 && m->hsync_end - m->hdisplay == 80 &&
           m->hsync_end - m->hsync_start == 32 && m->vsync_start - m->vdisplay == 3;
}

/** The DMT mode of size `h` by `v` at `fresh` Hz, with reduced blanking or
 * not as `rb` says, into `m`, Linux's drm_mode_find_dmt; 0 for none. */
static int find_dmt(int h, int v, int fresh, int rb, struct drm_mode_modeinfo *m)
{
    int i;

    for (i = 0; i < NELEM(dmt_modes); i++) {
        if (dmt_modes[i].hdisplay != h || dmt_modes[i].vdisplay != v) {
            continue;
        }
        mode_from_table(m, &dmt_modes[i]);
        if ((int)drm_mode_vrefresh(m) == fresh && mode_is_rb(m) == !!rb) {
            return 1;
        }
    }
    return 0;
}

/** Linux's drm_mode_hsync: the line rate in kHz, rounded. */
static int mode_hsync(const struct drm_mode_modeinfo *m)
{
    if (m->htotal == 0) {
        return 0;
    }
    return (int)((m->clock + m->htotal / 2u) / m->htotal);
}

/* ---- Detailed blocks ---- */

typedef void (*detailed_cb)(struct edid_parse *p, const uint8_t *d, void *closure);

/** Call `cb` on every 18-byte descriptor, Linux's
 * drm_for_each_detailed_block: the base block's four, then those of each
 * CTA and VTB extension. */
static void for_each_detailed(struct edid_parse *p, detailed_cb cb, void *closure)
{
    const uint8_t *ext;
    uint32_t b;
    int i, n, d;

    for (i = 0; i < 4; i++) {
        cb(p, p->edid + 0x36 + 18 * i, closure);
    }
    for (b = 1; b < p->blocks; b++) {
        ext = p->edid + b * EDID_LENGTH;
        if (ext[0] == CEA_EXT) {
            d = ext[2];
            if (d < 4 || d > 127) {
                continue;
            }
            n = (127 - d) / 18;
            for (i = 0; i < n; i++) {
                cb(p, ext + d + 18 * i, closure);
            }
        } else if (ext[0] == VTB_EXT && ext[1] == 1) {
            n = ext[2] < 6 ? ext[2] : 6;
            for (i = 0; i < n; i++) {
                cb(p, ext + 5 + 18 * i, closure);
            }
        }
    }
}

/** Whether `d` is a display descriptor of `type`. */
static int is_display(const uint8_t *d, uint8_t type)
{
    return d[0] == 0 && d[1] == 0 && d[2] == 0 && d[3] == type;
}

/** The closure of the walks that look for one range descriptor. */
struct range_find {
    int rb;              /* a CVT range with reduced blanking */
    int level;           /* the timing level the last range names */
    const uint8_t *gtf2; /* the last secondary-GTF range, or NULL */
};

/** Note what a range descriptor says, Linux's is_rb, find_gtf2 and
 * get_timing_level. */
static void find_range(struct edid_parse *p, const uint8_t *d, void *closure)
{
    struct range_find *f = (struct range_find *)closure;

    (void)p;
    if (!is_display(d, EDID_DETAIL_MONITOR_RANGE)) {
        return;
    }
    if (d[10] == RANGE_CVT && (d[15] & (1 << 4))) {
        f->rb = 1;
    }
    if (d[10] == RANGE_SECONDARY_GTF) {
        f->gtf2 = d;
    }
    switch (d[10]) {
    case RANGE_DEFAULT_GTF:
        f->level = LEVEL_GTF;
        break;
    case RANGE_SECONDARY_GTF:
        f->level = LEVEL_GTF2;
        break;
    case RANGE_CVT:
        f->level = LEVEL_CVT;
        break;
    default:
        break;
    }
}

/** Walk the range descriptors into `f`; its level starts at CVT, EDID 1.4's
 * default. */
static void ranges(struct edid_parse *p, struct range_find *f)
{
    f->rb = 0;
    f->level = LEVEL_CVT;
    f->gtf2 = NULL;
    for_each_detailed(p, find_range, f);
}

/** Linux's drm_monitor_supports_rb: EDID 1.4 says so in a CVT range; for
 * older ones, a digital input is taken to. */
static int supports_rb(struct edid_parse *p)
{
    struct range_find f;

    if (REVISION(p) >= 4) {
        ranges(p, &f);
        return f.rb;
    }
    return (p->edid[0x14] & 0x80) != 0;
}

/** Linux's drm_gtf2_hbreak: the line rate in kHz above which the secondary
 * GTF curve holds, 0 without one. */
static int gtf2_hbreak(struct edid_parse *p)
{
    struct range_find f;

    ranges(p, &f);
    return f.gtf2 ? f.gtf2[12] * 2 : 0;
}

/** Linux's standard_timing_level. */
static int timing_level(struct edid_parse *p)
{
    struct range_find f;

    if (REVISION(p) >= 4) {
        ranges(p, &f);
        return f.level;
    }
    if (REVISION(p) >= 3 && gtf2_hbreak(p)) {
        return LEVEL_GTF2;
    }
    return REVISION(p) >= 2 ? LEVEL_GTF : LEVEL_DMT;
}

/* ---- CVT and GTF, Linux's drm_modes.c ---- */

/**
 * The CVT mode of `hdisplay` by `vdisplay` at `vrefresh` Hz into `m`,
 * Linux's drm_cvt_mode, with its integer arithmetic: with reduced blanking,
 * interlace or margins as asked. 0 for a size of 0.
 */
int drm_cvt_mode(int hdisplay, int vdisplay, int vrefresh, int reduced, int interlaced, int margins,
                 struct drm_mode_modeinfo *m)
{
    unsigned int vfieldrate, hperiod;
    int hdisplay_rnd, hmargin, vdisplay_rnd, vmargin, vsync, interlace, tmp1, tmp2;
    uint64_t tmp;

    if (!hdisplay || !vdisplay) {
        return 0;
    }
    k_memset(m, 0, sizeof(*m));
    if (!vrefresh) {
        vrefresh = 60;
    }
    vfieldrate = (unsigned int)(interlaced ? vrefresh * 2 : vrefresh);
    hdisplay_rnd = hdisplay - (hdisplay % 8);
    hmargin = 0;
    if (margins) {
        hmargin = hdisplay_rnd * 18 / 1000;
        hmargin -= hmargin % 8;
    }
    m->hdisplay = (uint16_t)(hdisplay_rnd + 2 * hmargin);
    vdisplay_rnd = interlaced ? vdisplay / 2 : vdisplay;
    vmargin = margins ? vdisplay_rnd * 18 / 1000 : 0;
    m->vdisplay = (uint16_t)(vdisplay + 2 * vmargin);
    interlace = interlaced ? 1 : 0;
    if (!(vdisplay % 3) && vdisplay * 4 / 3 == hdisplay) {
        vsync = 4;
    } else if (!(vdisplay % 9) && vdisplay * 16 / 9 == hdisplay) {
        vsync = 5;
    } else if (!(vdisplay % 10) && vdisplay * 16 / 10 == hdisplay) {
        vsync = 6;
    } else if ((!(vdisplay % 4) && vdisplay * 5 / 4 == hdisplay) ||
               (!(vdisplay % 9) && vdisplay * 15 / 9 == hdisplay)) {
        vsync = 7;
    } else {
        vsync = 10;
    }
    if (!reduced) {
        unsigned int hblank_percentage;
        int vsyncandback_porch, hblank;

        tmp1 = (int)(1000u * 1000000u - 550u * 1000u * vfieldrate);
        tmp2 = (vdisplay_rnd + 2 * vmargin + 3) * 2 + interlace;
        hperiod = (unsigned int)tmp1 * 2u / ((unsigned int)tmp2 * vfieldrate);
        tmp1 = (int)(550u * 1000u / hperiod + 1u);
        vsyncandback_porch = tmp1 < vsync + 3 ? vsync + 3 : tmp1;
        m->vtotal = (uint16_t)(vdisplay_rnd + 2 * vmargin + vsyncandback_porch + 3);
        /* C' is 30 and M' 300 for CVT's M 600, C 40, K 128 and J 20. */
        hblank_percentage = 30u * 1000u - 300u * hperiod / 1000u;
        if (hblank_percentage < 20u * 1000u) {
            hblank_percentage = 20u * 1000u;
        }
        hblank = (int)(m->hdisplay * hblank_percentage / (100u * 1000u - hblank_percentage));
        hblank -= hblank % 16;
        m->htotal = (uint16_t)(m->hdisplay + hblank);
        m->hsync_end = (uint16_t)(m->hdisplay + hblank / 2);
        m->hsync_start = (uint16_t)(m->hsync_end - (m->htotal * 8) / 100);
        m->hsync_start = (uint16_t)(m->hsync_start + 8 - m->hsync_start % 8);
        m->vsync_start = (uint16_t)(m->vdisplay + 3);
        m->vsync_end = (uint16_t)(m->vsync_start + vsync);
    } else {
        int vbilines;

        tmp1 = (int)(1000u * 1000000u - 460u * 1000u * vfieldrate);
        tmp2 = vdisplay_rnd + 2 * vmargin;
        hperiod = (unsigned int)tmp1 / ((unsigned int)tmp2 * vfieldrate);
        vbilines = (int)(460u * 1000u / hperiod + 1u);
        if (vbilines < 3 + vsync + 6) {
            vbilines = 3 + vsync + 6;
        }
        m->vtotal = (uint16_t)(vdisplay_rnd + 2 * vmargin + vbilines);
        m->htotal = (uint16_t)(m->hdisplay + 160);
        m->hsync_end = (uint16_t)(m->hdisplay + 80);
        m->hsync_start = (uint16_t)(m->hsync_end - 32);
        m->vsync_start = (uint16_t)(m->vdisplay + 3);
        m->vsync_end = (uint16_t)(m->vsync_start + vsync);
    }
    /* Linux subtracts the clock's remainder by 250 before it sets the
     * clock, so nothing is subtracted. */
    tmp = (uint64_t)m->htotal * 1000u * 1000u;
    m->clock = (uint32_t)drm_div64(tmp, hperiod);
    if (interlaced) {
        m->vtotal = (uint16_t)(m->vtotal * 2);
        m->flags |= DRM_MODE_FLAG_INTERLACE;
    }
    drm_mode_set_name(m);
    m->flags |= reduced ? DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_NVSYNC
                        : DRM_MODE_FLAG_PVSYNC | DRM_MODE_FLAG_NHSYNC;
    m->type = 0;
    return 1;
}

/**
 * The GTF mode of `hdisplay` by `vdisplay` at `vrefresh` Hz into `m` with
 * the curve's parameters M, 2C, K and 2J, Linux's drm_gtf_mode_complex
 * with its integer arithmetic. 0 for a size of 0.
 */
static int gtf_mode_complex(int hdisplay, int vdisplay, int vrefresh, int interlaced, int margins,
                            int gtf_m, int gtf_2c, int gtf_k, int gtf_2j,
                            struct drm_mode_modeinfo *m)
{
    unsigned int hdisplay_rnd, vdisplay_rnd, vfieldrate_rqd, hfreq_est, vtotal_lines;
    unsigned int total_active_pixels, ideal_duty_cycle, hblank, total_pixels, pixel_freq;
    unsigned int tmp1, tmp2, c_prime, m_prime;
    int top_margin, interlace, vsync_plus_bp, left_margin, hsync, hfront_porch;

    if (!hdisplay || !vdisplay) {
        return 0;
    }
    k_memset(m, 0, sizeof(*m));
    c_prime = (unsigned int)((((gtf_2c - gtf_2j) * gtf_k / 256) + gtf_2j) / 2);
    m_prime = (unsigned int)(gtf_k * gtf_m / 256);
    hdisplay_rnd = ((unsigned int)hdisplay + 4u) / 8u * 8u;
    vdisplay_rnd = (unsigned int)(interlaced ? vdisplay / 2 : vdisplay);
    vfieldrate_rqd = (unsigned int)(interlaced ? vrefresh * 2 : vrefresh);
    top_margin = margins ? (int)((vdisplay_rnd * 18u + 500u) / 1000u) : 0;
    interlace = interlaced ? 1 : 0;
    tmp1 = (1000000u - 550u * vfieldrate_rqd) / 500u;
    tmp2 = (vdisplay_rnd + 2u * (unsigned int)top_margin + 1u) * 2u + (unsigned int)interlace;
    hfreq_est = (tmp2 * 1000u * vfieldrate_rqd) / tmp1;
    vsync_plus_bp = (int)(550u * hfreq_est / 1000u);
    vsync_plus_bp = (vsync_plus_bp + 500) / 1000;
    vtotal_lines = vdisplay_rnd + 2u * (unsigned int)top_margin + (unsigned int)vsync_plus_bp + 1u;
    left_margin = margins ? (int)((hdisplay_rnd * 18u + 500u) / 1000u) : 0;
    total_active_pixels = hdisplay_rnd + 2u * (unsigned int)left_margin;
    /* Linux's int product M' * 1000000 wraps as this unsigned one does. */
    ideal_duty_cycle = c_prime * 1000u - (m_prime * 1000000u) / hfreq_est;
    hblank = total_active_pixels * ideal_duty_cycle / (100000u - ideal_duty_cycle);
    hblank = (hblank + 8u) / 16u * 16u;
    total_pixels = total_active_pixels + hblank;
    pixel_freq = total_pixels * hfreq_est / 1000u;
    hsync = (int)(8u * total_pixels / 100u);
    hsync = (hsync + 4) / 8 * 8;
    hfront_porch = (int)(hblank / 2u) - hsync;
    m->hdisplay = (uint16_t)hdisplay_rnd;
    m->hsync_start = (uint16_t)(hdisplay_rnd + (unsigned int)hfront_porch);
    m->hsync_end = (uint16_t)(m->hsync_start + hsync);
    m->htotal = (uint16_t)total_pixels;
    m->vdisplay = (uint16_t)vdisplay_rnd;
    m->vsync_start = (uint16_t)(vdisplay_rnd + 1u);
    m->vsync_end = (uint16_t)(m->vsync_start + 3);
    m->vtotal = (uint16_t)vtotal_lines;
    m->clock = pixel_freq;
    if (interlaced) {
        m->vtotal = (uint16_t)(m->vtotal * 2);
        m->flags |= DRM_MODE_FLAG_INTERLACE;
    }
    drm_mode_set_name(m);
    if (gtf_m == 600 && gtf_2c == 80 && gtf_k == 128 && gtf_2j == 40) {
        m->flags = DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_PVSYNC;
    } else {
        m->flags = DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_NVSYNC;
    }
    return 1;
}

/** GTF's default curve, Linux's drm_gtf_mode. */
static int gtf_mode(int h, int v, int r, struct drm_mode_modeinfo *m)
{
    return gtf_mode_complex(h, v, r, 0, 0, 600, 80, 128, 40, m);
}

/** Linux's drm_gtf2_mode: the default curve below the EDID's break, its
 * secondary curve above. */
static int gtf2_mode(struct edid_parse *p, int h, int v, int r, struct drm_mode_modeinfo *m)
{
    struct range_find f;

    if (!gtf_mode(h, v, r, m)) {
        return 0;
    }
    if (mode_hsync(m) > gtf2_hbreak(p)) {
        ranges(p, &f);
        return gtf_mode_complex(h, v, r, 0, 0, f.gtf2 ? f.gtf2[14] | f.gtf2[15] << 8 : 0,
                                f.gtf2 ? f.gtf2[13] : 0, f.gtf2 ? f.gtf2[16] : 0,
                                f.gtf2 ? f.gtf2[17] : 0, m);
    }
    return 1;
}

/* ---- CEA and HDMI modes ---- */

/** The CEA mode of `vic`, or NULL for a VIC the tables lack. */
static const struct edid_mode *cea_mode_for_vic(uint32_t vic)
{
    if (vic >= 1 && vic < 1u + (uint32_t)NELEM(cea_modes_1)) {
        return &cea_modes_1[vic - 1];
    }
    if (vic >= 193 && vic < 193u + (uint32_t)NELEM(cea_modes_193)) {
        return &cea_modes_193[vic - 193];
    }
    return NULL;
}

/** The VIC after `vic` in the walk of all of them. */
static uint32_t cea_next_vic(uint32_t vic)
{
    return ++vic == 1u + (uint32_t)NELEM(cea_modes_1) ? 193 : vic;
}

#define CEA_NUM_VICS (193u + (uint32_t)NELEM(cea_modes_193))

/** Linux's cea_mode_alternate_clock: the 59.94 Hz clock of a 60 Hz mode,
 * or the 60 Hz one of the 240 and 480 line modes the tables keep at 59.94,
 * with Linux's 32-bit arithmetic. */
static uint32_t alternate_clock(const struct drm_mode_modeinfo *m)
{
    uint32_t clock = m->clock;

    if (drm_mode_vrefresh(m) % 6 != 0) {
        return clock;
    }
    if (m->vdisplay == 240 || m->vdisplay == 480) {
        return (clock * 1001u + 500u) / 1000u;
    }
    return (clock * 1000u + 500u) / 1001u;
}

/** Linux's cea_mode_alternate_timings: the variant of some VICs with a
 * longer front porch; 0 when there is none left. */
static int alternate_timings(uint32_t vic, struct drm_mode_modeinfo *m)
{
    if (((vic == 8 || vic == 9 || vic == 12 || vic == 13) && m->vtotal < 263) ||
        ((vic == 23 || vic == 24 || vic == 27 || vic == 28) && m->vtotal < 314)) {
        m->vsync_start++;
        m->vsync_end++;
        m->vtotal++;
        return 1;
    }
    return 0;
}

/** Linux's drm_mode_match of the timings and flags, and of the aspect when
 * `a` has one. */
static int mode_match(const struct drm_mode_modeinfo *a, const struct drm_mode_modeinfo *b)
{
    uint32_t mask = ~(uint32_t)(DRM_MODE_FLAG_3D_MASK | DRM_MODE_FLAG_PIC_AR_MASK);

    if (a->hdisplay != b->hdisplay || a->hsync_start != b->hsync_start ||
        a->hsync_end != b->hsync_end || a->htotal != b->htotal || a->hskew != b->hskew ||
        a->vdisplay != b->vdisplay || a->vsync_start != b->vsync_start ||
        a->vsync_end != b->vsync_end || a->vtotal != b->vtotal || a->vscan != b->vscan) {
        return 0;
    }
    if ((a->flags & mask) != (b->flags & mask)) {
        return 0;
    }
    return !(a->flags & DRM_MODE_FLAG_PIC_AR_MASK) ||
           (a->flags & DRM_MODE_FLAG_PIC_AR_MASK) == (b->flags & DRM_MODE_FLAG_PIC_AR_MASK);
}

/** Linux's KHZ2PICOS: a clock's period in picoseconds. */
#define KHZ2PICOS(a) (1000000000u / (a))

/** Whether clock `c` is near enough `c1` or `c2`: within `tol` kHz, or for
 * a negative `tol` of the same period in picoseconds. */
static int clock_near(uint32_t c, uint32_t c1, uint32_t c2, int tol)
{
    if (tol < 0) {
        return (c1 && KHZ2PICOS(c) == KHZ2PICOS(c1)) || (c2 && KHZ2PICOS(c) == KHZ2PICOS(c2));
    }
    return (c > c1 ? c - c1 : c1 - c) <= (uint32_t)tol ||
           (c > c2 ? c - c2 : c2 - c) <= (uint32_t)tol;
}

/** The VIC of the CEA mode `m` is, Linux's drm_match_cea_mode for a
 * negative `tol` and drm_match_cea_mode_clock_tolerance otherwise; 0 for
 * none. */
static uint32_t match_cea(const struct drm_mode_modeinfo *m, int tol)
{
    struct drm_mode_modeinfo c;
    uint32_t vic;

    if (!m->clock) {
        return 0;
    }
    for (vic = 1; vic < CEA_NUM_VICS; vic = cea_next_vic(vic)) {
        mode_from_table(&c, cea_mode_for_vic(vic));
        if (!clock_near(m->clock, c.clock, alternate_clock(&c), tol)) {
            continue;
        }
        do {
            if (mode_match(m, &c)) {
                return vic;
            }
        } while (alternate_timings(vic, &c));
    }
    return 0;
}

/** The HDMI VIC of the mode `m` is, Linux's drm_match_hdmi_mode and its
 * clock-tolerance form, as `match_cea`; 0 for none. */
static uint32_t match_hdmi(const struct drm_mode_modeinfo *m, int tol)
{
    struct drm_mode_modeinfo h;
    uint32_t vic;

    if (!m->clock) {
        return 0;
    }
    for (vic = 1; vic < (uint32_t)NELEM(hdmi_modes); vic++) {
        mode_from_table(&h, &hdmi_modes[vic]);
        if (clock_near(m->clock, h.clock, alternate_clock(&h), tol) && mode_match(m, &h)) {
            return vic;
        }
    }
    return 0;
}

/** Linux's fixup_detailed_cea_mode_clock: give a detailed mode that is a
 * CEA or HDMI mode within 5 kHz the exact clock, the nearer of the two. */
static void fixup_cea_clock(struct drm_mode_modeinfo *m)
{
    struct drm_mode_modeinfo c;
    uint32_t vic = match_cea(m, 5), c1, c2;

    if (vic) {
        mode_from_table(&c, cea_mode_for_vic(vic));
    } else if ((vic = match_hdmi(m, 5)) != 0) {
        mode_from_table(&c, &hdmi_modes[vic]);
    } else {
        return;
    }
    c1 = c.clock;
    c2 = alternate_clock(&c);
    if ((m->clock > c1 ? m->clock - c1 : c1 - m->clock) <
        (m->clock > c2 ? m->clock - c2 : c2 - m->clock)) {
        m->clock = c1;
    } else {
        m->clock = c2;
    }
}

/* ---- Each kind of mode, in Linux's order ---- */

/** Linux's drm_mode_detailed: the mode of a detailed timing descriptor, 0
 * for a tiny, stereo or sync-less one. EDID quirks are not applied. */
static int mode_detailed(const uint8_t *d, struct drm_mode_modeinfo *m)
{
    static const uint16_t interlaced[][2] = {{1920, 1080}, {720, 480},  {1440, 480}, {2880, 480},
                                             {720, 576},   {1440, 576}, {2880, 576}};
    unsigned int hactive = (unsigned int)(d[4] & 0xf0) << 4 | d[2];
    unsigned int vactive = (unsigned int)(d[7] & 0xf0) << 4 | d[5];
    unsigned int hblank = (unsigned int)(d[4] & 0xf) << 8 | d[3];
    unsigned int vblank = (unsigned int)(d[7] & 0xf) << 8 | d[6];
    unsigned int hsync_offset = (unsigned int)(d[11] & 0xc0) << 2 | d[8];
    unsigned int hsync_width = (unsigned int)(d[11] & 0x30) << 4 | d[9];
    unsigned int vsync_offset = (unsigned int)(d[11] & 0xc) << 2 | d[10] >> 4;
    unsigned int vsync_width = (unsigned int)(d[11] & 0x3) << 4 | (d[10] & 0xf);
    int i;

    if (hactive < 64 || vactive < 64 || (d[17] & PT_STEREO) || !hsync_width || !vsync_width) {
        return 0;
    }
    k_memset(m, 0, sizeof(*m));
    m->clock = (uint32_t)(d[0] | d[1] << 8) * 10u;
    m->hdisplay = (uint16_t)hactive;
    m->hsync_start = (uint16_t)(hactive + hsync_offset);
    m->hsync_end = (uint16_t)(m->hsync_start + hsync_width);
    m->htotal = (uint16_t)(hactive + hblank);
    m->vdisplay = (uint16_t)vactive;
    m->vsync_start = (uint16_t)(vactive + vsync_offset);
    m->vsync_end = (uint16_t)(m->vsync_start + vsync_width);
    m->vtotal = (uint16_t)(vactive + vblank);
    if (m->hsync_end > m->htotal) {
        m->hsync_end = m->htotal;
    }
    if (m->vsync_end > m->vtotal) {
        m->vsync_end = m->vtotal;
    }
    /* Linux's drm_mode_do_interlace_quirk: some interlaced CEA timings give
     * the field's height. */
    if (d[17] & PT_INTERLACED) {
        for (i = 0; i < NELEM(interlaced); i++) {
            if (m->hdisplay == interlaced[i][0] && m->vdisplay == interlaced[i][1] / 2) {
                m->vdisplay = (uint16_t)(m->vdisplay * 2);
                m->vsync_start = (uint16_t)(m->vsync_start * 2);
                m->vsync_end = (uint16_t)(m->vsync_end * 2);
                m->vtotal = (uint16_t)(m->vtotal * 2 | 1);
            }
        }
        m->flags |= DRM_MODE_FLAG_INTERLACE;
    }
    m->flags |= (d[17] & PT_HSYNC_POSITIVE) ? DRM_MODE_FLAG_PHSYNC : DRM_MODE_FLAG_NHSYNC;
    m->flags |= (d[17] & PT_VSYNC_POSITIVE) ? DRM_MODE_FLAG_PVSYNC : DRM_MODE_FLAG_NVSYNC;
    m->type = DRM_MODE_TYPE_DRIVER;
    drm_mode_set_name(m);
    return 1;
}

/** Linux's do_detailed_mode: add a detailed timing's mode, the first one
 * preferred when `*preferred` says so. */
static void do_detailed(struct edid_parse *p, const uint8_t *d, void *closure)
{
    int *preferred = (int *)closure;
    struct drm_mode_modeinfo m;

    if ((d[0] | d[1]) == 0 || !mode_detailed(d, &m)) {
        return;
    }
    if (*preferred) {
        m.type |= DRM_MODE_TYPE_PREFERRED;
    }
    fixup_cea_clock(&m);
    parse_add(p, &m);
    *preferred = 0;
}

/** Linux's do_cvt_mode and drm_cvt_modes: the CVT 3-byte codes. */
static void do_cvt(struct edid_parse *p, const uint8_t *d, void *closure)
{
    static const int rates[] = {60, 85, 75, 60, 50};
    struct drm_mode_modeinfo m;
    const uint8_t *code;
    int i, j, width, height;

    (void)closure;
    if (!is_display(d, EDID_DETAIL_CVT_3BYTE)) {
        return;
    }
    for (i = 0; i < 4; i++) {
        code = d + 6 + 3 * i;
        if (!code[0] && !code[1] && !code[2]) {
            continue;
        }
        height = (code[0] + ((code[1] & 0xf0) << 4) + 1) * 2;
        switch (code[1] & 0x0c) {
        case 0x04:
            width = height * 16 / 9;
            break;
        case 0x08:
            width = height * 16 / 10;
            break;
        case 0x0c:
            width = height * 15 / 9;
            break;
        default:
            width = height * 4 / 3;
            break;
        }
        /* Linux's loop starts at bit 1, so it never asks for the reduced
         * blanking that bit 0 offers. */
        for (j = 1; j < 5; j++) {
            if ((code[2] & (1 << j)) && drm_cvt_mode(width, height, rates[j], 0, 0, 0, &m)) {
                parse_add(p, &m);
            }
        }
    }
}

/** Linux's drm_mode_std: the mode of a standard timing's two bytes, 0 for
 * none or one already found. */
static int mode_std(struct edid_parse *p, uint8_t hbyte, uint8_t vbyte, struct drm_mode_modeinfo *m)
{
    unsigned int aspect = (vbyte & 0xc0) >> 6;
    int hsize, vsize, refresh;
    const struct drm_display_mode *e;

    if ((hbyte == 0x00 && vbyte == 0x00) || (hbyte == 0x01 && vbyte == 0x01) ||
        (hbyte == 0x20 && vbyte == 0x20)) {
        return 0;
    }
    hsize = hbyte * 8 + 248;
    refresh = (vbyte & 0x3f) + 60;
    if (aspect == 0) {
        vsize = REVISION(p) < 3 ? hsize : hsize * 10 / 16;
    } else if (aspect == 1) {
        vsize = hsize * 3 / 4;
    } else if (aspect == 2) {
        vsize = hsize * 4 / 5;
    } else {
        vsize = hsize * 9 / 16;
    }
    /* Linux's HDTV hack: 1366 is not a multiple of 8. */
    if (refresh == 60 && ((hsize == 1360 && vsize == 765) || (hsize == 1368 && vsize == 769))) {
        hsize = 1366;
        vsize = 768;
    }
    for (e = p->head; e; e = e->next) {
        if (e->info.hdisplay == hsize && e->info.vdisplay == vsize &&
            (int)drm_mode_vrefresh(&e->info) == refresh) {
            return 0;
        }
    }
    if (hsize == 1366 && vsize == 768 && refresh == 60) {
        if (!drm_cvt_mode(1366, 768, refresh, 0, 0, 0, m)) {
            return 0;
        }
        m->hdisplay = 1366;
        m->hsync_start--;
        m->hsync_end--;
        return 1;
    }
    if ((supports_rb(p) && find_dmt(hsize, vsize, refresh, 1, m)) ||
        find_dmt(hsize, vsize, refresh, 0, m)) {
        return 1;
    }
    switch (timing_level(p)) {
    case LEVEL_GTF:
        return gtf_mode(hsize, vsize, refresh, m);
    case LEVEL_GTF2:
        return gtf2_mode(p, hsize, vsize, refresh, m);
    case LEVEL_CVT:
        return drm_cvt_mode(hsize, vsize, refresh, 0, 0, 0, m);
    default:
        return 0;
    }
}

/** Linux's do_standard_modes: the standard timings of a descriptor. */
static void do_std(struct edid_parse *p, const uint8_t *d, void *closure)
{
    struct drm_mode_modeinfo m;
    int i;

    (void)closure;
    if (!is_display(d, EDID_DETAIL_STD_MODES)) {
        return;
    }
    for (i = 0; i < 6; i++) {
        if (mode_std(p, d[5 + 2 * i], d[6 + 2 * i], &m)) {
            parse_add(p, &m);
        }
    }
}

/** Linux's do_established_modes and drm_est3_modes: the established
 * timings III. */
static void do_est3(struct edid_parse *p, const uint8_t *d, void *closure)
{
    struct drm_mode_modeinfo m;
    int i, j, k;

    (void)closure;
    if (!is_display(d, EDID_DETAIL_EST_TIMINGS)) {
        return;
    }
    for (i = 0; i < 6; i++) {
        for (j = 7; j >= 0; j--) {
            k = i * 8 + (7 - j);
            if (k >= NELEM(est3_modes)) {
                break;
            }
            if ((d[6 + i] & (1 << j)) &&
                find_dmt(est3_modes[k].w, est3_modes[k].h, est3_modes[k].r, est3_modes[k].rb, &m)) {
                parse_add(p, &m);
            }
        }
    }
}

/** Add the mode of CEA VIC `vic`, if the tables have it. */
static void add_vic(struct edid_parse *p, uint32_t vic)
{
    struct drm_mode_modeinfo m;
    const struct edid_mode *t = cea_mode_for_vic(vic);

    if (t) {
        mode_from_table(&m, t);
        parse_add(p, &m);
    }
}

/** Linux's svd_to_vic: a short video descriptor's VIC, without the native
 * bit where the code has one. */
static uint32_t svd_to_vic(uint8_t svd)
{
    return (svd >= 1 && svd <= 64) || (svd >= 129 && svd <= 192) ? svd & 127u : svd;
}

typedef void (*cta_db_cb)(struct edid_parse *p, const uint8_t *db, uint32_t len);

/** Call `cb` on every data block of every CTA extension of revision 3 or
 * later with the block's payload length, Linux's cea_db_iter over the EDID's
 * extensions. */
static void for_each_cta_db(struct edid_parse *p, cta_db_cb cb)
{
    const uint8_t *ext;
    uint32_t b, at, end, len;

    for (b = 1; b < p->blocks; b++) {
        ext = p->edid + b * EDID_LENGTH;
        if (ext[0] != CEA_EXT || ext[1] < 3 || ext[2] < 4 || ext[2] > 127) {
            continue;
        }
        end = ext[2];
        for (at = 4; at + 1 <= end; at += 1 + len) {
            len = ext[at] & 0x1fu;
            if (at + 1 + len > end) {
                break;
            }
            cb(p, ext + at, len);
        }
    }
}

/** Linux's parse_cta_vdb: keep the VICs of a video data block. */
static void parse_vdb(struct edid_parse *p, const uint8_t *db, uint32_t len)
{
    uint32_t i, vic;

    if (db[0] >> 5 != 2) {
        return;
    }
    for (i = 0; i < len && p->nvics < sizeof(p->vics); i++) {
        vic = svd_to_vic(db[1 + i]);
        p->vics[p->nvics++] = (uint8_t)(cea_mode_for_vic(vic) ? vic : 0);
    }
}

/** Linux's add_cea_modes beyond the video data blocks: the HDMI vendor
 * block's 4K VICs, and the VICs of a 4:2:0 video data block. The HDMI
 * block's stereo modes are left out, as no connector takes stereo. */
static void do_cta_modes(struct edid_parse *p, const uint8_t *db, uint32_t len)
{
    struct drm_mode_modeinfo m;
    uint32_t i, offset = 0, vic_len, vic;
    int tag = db[0] >> 5;

    if (tag == 3 && len >= 5 && db[1] == 0x03 && db[2] == 0x0c && db[3] == 0x00) {
        /* HDMI_Video_Present, then the latencies Linux skips. */
        if (len < 8 || !(db[8] & (1 << 5))) {
            return;
        }
        if (db[8] & (1 << 7)) {
            offset += (db[8] & (1 << 6)) ? 4 : 2;
        }
        if (len < 8 + offset + 2) {
            return;
        }
        offset += 2;
        vic_len = (uint32_t)db[8 + offset] >> 5;
        for (i = 0; i < vic_len && len >= 9 + offset + i; i++) {
            vic = db[9 + offset + i];
            if (vic > 0 && vic < (uint32_t)NELEM(hdmi_modes)) {
                mode_from_table(&m, &hdmi_modes[vic]);
                parse_add(p, &m);
            }
        }
    } else if (tag == 7 && len >= 1 && db[1] == 14) {
        for (i = 0; i + 1 < len; i++) {
            add_vic(p, svd_to_vic(db[2 + i]));
        }
    }
}

/** Linux's add_alternate_cea_modes: for each CEA or HDMI mode found, add
 * its twin at the other of 60 and 59.94 Hz, after all of them. */
static void add_alternates(struct edid_parse *p)
{
    struct drm_display_mode *e, *last = NULL;
    struct drm_mode_modeinfo t, n;
    uint32_t vic, c1, c2, b;
    int cta = 0;

    for (b = 1; b < p->blocks; b++) {
        cta |= p->edid[b * EDID_LENGTH] == CEA_EXT;
    }
    if (!cta) {
        return;
    }
    for (e = p->head; e; e = e->next) {
        last = e;
    }
    for (e = p->head; e; e = e->next) {
        if ((vic = match_cea(&e->info, -1)) != 0) {
            mode_from_table(&t, cea_mode_for_vic(vic));
        } else if ((vic = match_hdmi(&e->info, -1)) != 0) {
            mode_from_table(&t, &hdmi_modes[vic]);
        } else {
            if (e == last) {
                break;
            }
            continue;
        }
        c1 = t.clock;
        c2 = alternate_clock(&t);
        if (c1 != c2 && (e->info.clock == c1 || e->info.clock == c2)) {
            n = t;
            n.flags |= e->info.flags & DRM_MODE_FLAG_3D_MASK;
            n.clock = e->info.clock != c1 ? c1 : c2;
            parse_add(p, &n);
        }
        if (e == last) {
            break;
        }
    }
}

/* ---- Modes inferred from a range descriptor ---- */

/** Linux's mode_in_range: whether a mode fits the range descriptor `t`. */
static int mode_in_range(struct edid_parse *p, const struct drm_mode_modeinfo *m, const uint8_t *t)
{
    int hmin = t[7], hmax = t[8], vmin = t[5], vmax = t[6], hsync, vsync;
    uint32_t max_clock = 0;

    if (REVISION(p) >= 4) {
        hmin += (t[4] & 0x04) ? 255 : 0;
        hmax += (t[4] & 0x08) ? 255 : 0;
        vmin += (t[4] & 0x01) ? 255 : 0;
        vmax += (t[4] & 0x02) ? 255 : 0;
    }
    hsync = mode_hsync(m);
    vsync = (int)drm_mode_vrefresh(m);
    if (hsync > hmax || hsync < hmin || vsync > vmax || vsync < vmin) {
        return 0;
    }
    if (t[9] != 0 && t[9] != 255) {
        max_clock = REVISION(p) >= 4 && t[10] == RANGE_CVT
                        ? t[9] * 10000u - (uint32_t)(t[12] >> 2) * 250u
                        : t[9] * 10000u + 5001u;
    }
    if (max_clock && m->clock > max_clock) {
        return 0;
    }
    if (REVISION(p) >= 4 && t[10] == RANGE_CVT && t[13] &&
        m->hdisplay > 8 * (t[13] + 256 * (t[12] & 0x3))) {
        return 0;
    }
    return !(mode_is_rb(m) && !supports_rb(p));
}

/** Linux's valid_inferred_mode: not found already, and no larger than some
 * mode found. */
static int valid_inferred(struct edid_parse *p, const struct drm_mode_modeinfo *m)
{
    const struct drm_display_mode *e;
    int ok = 0;

    for (e = p->head; e; e = e->next) {
        if (m->hdisplay == e->info.hdisplay && m->vdisplay == e->info.vdisplay &&
            drm_mode_vrefresh(m) == drm_mode_vrefresh(&e->info)) {
            return 0;
        }
        if (m->hdisplay <= e->info.hdisplay && m->vdisplay <= e->info.vdisplay) {
            ok = 1;
        }
    }
    return ok;
}

/** Linux's do_inferred_modes: the DMT modes inside a range, then the sizes
 * of `extra_modes` by the range's formula. */
static void do_inferred(struct edid_parse *p, const uint8_t *t, void *closure)
{
    struct drm_mode_modeinfo m;
    int i, ok, rb = 0;

    (void)closure;
    if (!is_display(t, EDID_DETAIL_MONITOR_RANGE)) {
        return;
    }
    for (i = 0; i < NELEM(dmt_modes); i++) {
        mode_from_table(&m, &dmt_modes[i]);
        if (mode_in_range(p, &m, t) && valid_inferred(p, &m)) {
            parse_add(p, &m);
        }
    }
    if (REVISION(p) < 2) {
        return;
    }
    if (t[10] == RANGE_CVT) {
        if (REVISION(p) < 4) {
            return;
        }
        rb = supports_rb(p);
    } else if (t[10] != RANGE_DEFAULT_GTF && t[10] != RANGE_SECONDARY_GTF) {
        return;
    }
    for (i = 0; i < NELEM(extra_modes); i++) {
        if (t[10] == RANGE_CVT) {
            ok = drm_cvt_mode(extra_modes[i].w, extra_modes[i].h, extra_modes[i].r, rb, 0, 0, &m);
        } else if (t[10] == RANGE_SECONDARY_GTF) {
            ok = gtf2_mode(p, extra_modes[i].w, extra_modes[i].h, extra_modes[i].r, &m);
        } else {
            ok = gtf_mode(extra_modes[i].w, extra_modes[i].h, extra_modes[i].r, &m);
        }
        if (!ok) {
            return;
        }
        /* Linux's drm_mode_fixup_1366x768. */
        if (m.hdisplay == 1368 && m.vdisplay == 768) {
            m.hdisplay = 1366;
            m.hsync_start--;
            m.hsync_end--;
            drm_mode_set_name(&m);
        }
        if (mode_in_range(p, &m, t) && valid_inferred(p, &m)) {
            parse_add(p, &m);
        }
    }
}

/**
 * Find the modes of a cleaned EDID, Linux's _drm_edid_connector_add_modes:
 * the detailed timings, the CVT codes, the standard timings, the
 * established timings, the CTA modes and their alternate clocks, and with
 * continuous frequencies the modes the range limits allow. DisplayID
 * blocks are not read.
 */
static void parse_modes(struct edid_parse *p)
{
    struct drm_mode_modeinfo m;
    uint32_t est, i;
    int preferred;

    preferred = REVISION(p) >= 4 || (p->edid[0x18] & (1 << 1));
    for_each_detailed(p, do_detailed, &preferred);
    if (REVISION(p) >= 3) {
        for_each_detailed(p, do_cvt, NULL);
    }
    for (i = 0; i < 8; i++) {
        if (mode_std(p, p->edid[0x26 + 2 * i], p->edid[0x27 + 2 * i], &m)) {
            parse_add(p, &m);
        }
    }
    if (REVISION(p) >= 1) {
        for_each_detailed(p, do_std, NULL);
    }
    est = p->edid[0x23] | (uint32_t)p->edid[0x24] << 8 | (uint32_t)(p->edid[0x25] & 0x80) << 9;
    for (i = 0; i <= 16; i++) {
        if (est & (1u << i)) {
            mode_from_table(&m, &est_modes[i]);
            parse_add(p, &m);
        }
    }
    if (REVISION(p) >= 1) {
        for_each_detailed(p, do_est3, NULL);
    }
    for_each_cta_db(p, parse_vdb);
    for (i = 0; i < p->nvics; i++) {
        if (p->vics[i]) {
            add_vic(p, p->vics[i]);
        }
    }
    for_each_cta_db(p, do_cta_modes);
    add_alternates(p);
    if ((p->edid[0x18] & 1) && REVISION(p) >= 1) {
        for_each_detailed(p, do_inferred, NULL);
    }
}

/** Free the modes a parse found. */
static void parse_free(struct edid_parse *p)
{
    struct drm_display_mode *e;

    while ((e = p->head) != NULL) {
        p->head = e->next;
        kfree(e);
    }
}

/** Clean `edid` and find its modes into `*p`, kmalloc'd; the cleaned copy
 * is returned, or NULL for an EDID that is not valid. */
static uint8_t *parse(const uint8_t *edid, uint32_t len, struct edid_parse *p)
{
    uint8_t *clean;

    k_memset(p, 0, sizeof(*p));
    p->tail = &p->head;
    clean = edid_clean(edid, len, &p->blocks);
    if (clean) {
        p->edid = clean;
        parse_modes(p);
    }
    return clean;
}

int drm_edid_parse_modes(const uint8_t *edid, uint32_t len,
                         int (*add)(const struct drm_mode_modeinfo *mode, void *arg), void *arg)
{
    struct edid_parse *p = (struct edid_parse *)kmalloc(sizeof(*p));
    const struct drm_display_mode *e;
    uint8_t *clean;
    int n = 0;

    if (!p) {
        return 0;
    }
    clean = parse(edid, len, p);
    for (e = p->head; e; e = e->next) {
        if (add(&e->info, arg) == 0) {
            n++;
        }
    }
    parse_free(p);
    kfree(clean);
    kfree(p);
    return n;
}

/** Add one mode to the connector `arg`'s probed modes. */
static int add_probed(const struct drm_mode_modeinfo *mode, void *arg)
{
    return drm_mode_probed_add((struct drm_connector *)arg, mode) == 0 ? 0 : -1;
}

int drm_connector_update_edid(struct drm_connector *connector, const uint8_t *edid, uint32_t len)
{
    struct drm_device *dev = connector->dev;
    struct drm_property_blob *blob = NULL;
    uint32_t blocks = 0;
    uint8_t *clean;
    int32_t err = 0;
    int n = 0;

    if (connector->edid_override) {
        edid = connector->edid_override;
        len = connector->edid_override_len;
    }
    clean = edid_clean(edid, len, &blocks);
    /* A new blob each time, made before the old one goes, as Linux's
     * drm_property_replace_global_blob makes it. */
    if (clean) {
        blob = drm_property_create_blob(dev, blocks * EDID_LENGTH, clean, &err);
    }
    drm_property_blob_put(connector->edid);
    connector->edid = blob;
    drm_object_property_set_value(&connector->base, dev->mode_config.edid_property,
                                  blob ? blob->base.id : 0);
    connector->mm_width = clean ? clean[0x15] * 10u : 0;
    connector->mm_height = clean ? clean[0x16] * 10u : 0;
    if (clean) {
        n = drm_edid_parse_modes(clean, blocks * EDID_LENGTH, add_probed, connector);
    }
    kfree(clean);
    return n;
}

uint32_t drm_match_cea_mode(const struct drm_mode_modeinfo *mode)
{
    return match_cea(mode, -1);
}

uint32_t drm_match_hdmi_mode(const struct drm_mode_modeinfo *mode)
{
    return match_hdmi(mode, -1);
}

/** HDMI's vendor block `db` of `len` bytes, Linux's drm_parse_hdmi_vsdb_video
 * and drm_parse_hdmi_deep_color_info: the deep colour bits set the bits a
 * colour to the most they name, and leave it when none is set. */
static void do_hdmi_vsdb(struct drm_edid_hdmi_info *info, const uint8_t *db, uint32_t len)
{
    info->is_hdmi = 1;
    if (len >= 6 && (db[6] & (1 << 4))) {
        info->dc_30 = 1;
        info->bpc = 10;
    }
    if (len >= 6 && (db[6] & (1 << 5))) {
        info->dc_36 = 1;
        info->bpc = 12;
    }
    if (len >= 6 && (db[6] & (1 << 6))) {
        info->bpc = 16;
    }
    if (len >= 7) {
        info->max_tmds_clock = db[7] * 5000u;
    }
    if (len >= 8 && (db[8] & (1 << 5))) {
        info->has_hdmi_infoframe = 1;
    }
}

/** Linux's drm_detect_hdmi_monitor, drm_detect_monitor_audio and its
 * parse of the HDMI blocks, the video capability block and the 4:2:0
 * blocks, from one CTA data block of `len` payload bytes into `p->hdmi`. */
static void do_hdmi_info(struct edid_parse *p, const uint8_t *db, uint32_t len)
{
    struct drm_edid_hdmi_info *info = p->hdmi;
    uint32_t i, vic;
    int tag = db[0] >> 5;

    if (tag == 3 && len >= 5 && db[1] == 0x03 && db[2] == 0x0c && db[3] == 0x00) {
        do_hdmi_vsdb(info, db, len);
    } else if (tag == 3 && len >= 7 && db[1] == 0xd8 && db[2] == 0x5d && db[3] == 0xc4) {
        /* HDMI Forum's block: SCDC_Present in its sixth byte. */
        info->has_hdmi_infoframe = 1;
        if (db[6] & 0x80) {
            info->hdmi2 = 1;
        }
    } else if (tag == 1 && len > 0) {
        info->has_audio = 1;
    } else if (tag == 2) {
        for (i = 0; i < len; i++) {
            vic = svd_to_vic(db[1 + i]);
            info->vics[vic >> 3] |= (uint8_t)(1u << (vic & 7u));
        }
    } else if (tag == 7 && len == 2 && db[1] == 0) {
        if (db[2] & (1 << 6)) {
            info->rgb_quant_selectable = 1;
        }
    } else if (tag == 7 && len >= 1 && (db[1] == 14 || db[1] == 15)) {
        /* A 4:2:0 video or capability map block. */
        info->hdmi2 = 1;
    }
}

/* The bits a colour of EDID 1.4's digital depth field; 0 undefined. */
static const uint8_t edid_depth[8] = {0, 6, 8, 10, 12, 14, 16, 0};

void drm_edid_hdmi_info(const struct drm_connector *connector, const uint8_t *edid, uint32_t len,
                        struct drm_edid_hdmi_info *info)
{
    struct edid_parse p;
    uint8_t *clean;
    uint32_t b;

    k_memset(info, 0, sizeof(*info));
    if (connector && connector->edid_override) {
        edid = connector->edid_override;
        len = connector->edid_override_len;
    }
    k_memset(&p, 0, sizeof(p));
    clean = edid ? edid_clean(edid, len, &p.blocks) : NULL;
    if (!clean) {
        return;
    }
    p.edid = clean;
    info->digital = (clean[0x14] & 0x80) != 0;
    /* Linux's update_display_info reads the extensions of a digital
     * EDID 1.3 or later alone. */
    if (clean[0x13] < 3 || !info->digital) {
        kfree(clean);
        return;
    }
    for (b = 1; b < p.blocks; b++) {
        if (clean[b * EDID_LENGTH] == CEA_EXT && clean[b * EDID_LENGTH + 1] >= 2 &&
            (clean[b * EDID_LENGTH + 3] & (1 << 6))) {
            info->has_audio = 1;
        }
    }
    p.hdmi = info;
    for_each_cta_db(&p, do_hdmi_info);
    /* A DFP 1.x sink of EDID 1.3 takes 8 bits; EDID 1.4 names its depth,
     * which stands over the HDMI block's. */
    if (info->bpc == 0 && clean[0x13] == 3 && (clean[0x14] & 1)) {
        info->bpc = 8;
    }
    if (clean[0x13] >= 4) {
        info->bpc = edid_depth[(clean[0x14] >> 4) & 7u];
    }
    kfree(clean);
}

int drm_default_rgb_quant_limited(const struct drm_mode_modeinfo *mode)
{
    return match_cea(mode, -1) > 1;
}

/* The picture aspects of an AVI infoframe, Linux's enum hdmi_picture_aspect,
 * which are DRM's. */
#define AVI_ASPECT_NONE 0
#define AVI_ASPECT_16_9 2

/** The aspect of the mode's flags, 0 for none. */
static uint32_t mode_aspect(uint32_t flags)
{
    return (flags & DRM_MODE_FLAG_PIC_AR_MASK) >> 19;
}

/** Whether VIC `vic` is in one of the sink's video data blocks. */
static int info_has_vic(const struct drm_edid_hdmi_info *info, uint32_t vic)
{
    return vic < 256 && (info->vics[vic >> 3] & (1u << (vic & 7u))) != 0;
}

/** Set the checksum of the infoframe of `len` bytes at `buf`, its fourth
 * byte, so that the bytes sum to 0. */
static void infoframe_checksum(uint8_t *buf, uint32_t len)
{
    uint8_t sum = 0;
    uint32_t i;

    buf[3] = 0;
    for (i = 0; i < len; i++) {
        sum = (uint8_t)(sum + buf[i]);
    }
    buf[3] = (uint8_t)(256u - sum);
}

/** Linux's drm_mode_hdmi_vic: the HDMI VIC of `mode` for a sink that takes
 * HDMI's vendor infoframe, 0 otherwise. */
static uint32_t mode_hdmi_vic(const struct drm_edid_hdmi_info *info,
                              const struct drm_mode_modeinfo *mode)
{
    if (!info->has_hdmi_infoframe || (mode->flags & DRM_MODE_FLAG_3D_MASK)) {
        return 0;
    }
    return match_hdmi(mode, -1);
}

uint32_t drm_hdmi_avi_infoframe(const struct drm_edid_hdmi_info *info,
                                const struct drm_mode_modeinfo *mode, int limited, uint8_t *buf)
{
    uint32_t vic, hdmi_vic, aspect, quant, ycc;

    hdmi_vic = mode_hdmi_vic(info, mode);
    vic = hdmi_vic ? 0 : match_cea(mode, -1);
    aspect = mode_aspect(mode->flags);
    if (aspect == AVI_ASPECT_NONE) {
        if (vic) {
            aspect = mode_aspect(cea_mode_for_vic(vic)->flags);
        } else if (hdmi_vic) {
            aspect = mode_aspect(hdmi_modes[hdmi_vic].flags);
        }
    }
    /* The frame names no aspect past 16:9; such a mode says it by VIC. */
    if (aspect > AVI_ASPECT_16_9) {
        if (vic) {
            if (aspect != mode_aspect(cea_mode_for_vic(vic)->flags)) {
                return 0;
            }
        } else if (hdmi_vic) {
            if (aspect != mode_aspect(hdmi_modes[hdmi_vic].flags)) {
                return 0;
            }
        } else {
            return 0;
        }
        aspect = AVI_ASPECT_NONE;
    }
    /* Linux's vic_for_avi_infoframe: a VIC past 64 only for an HDMI 2.0
     * sink or one that lists it. */
    if (!info->hdmi2 && vic > 64 && !info_has_vic(info, vic)) {
        vic = 0;
    }
    /* Linux's drm_hdmi_avi_infoframe_quant_range: Q only when the sink
     * takes it or it is the mode's default; YQ full only for HDMI 2.0. */
    quant = (info->rgb_quant_selectable || limited == drm_default_rgb_quant_limited(mode))
                ? (limited ? 1u : 2u)
                : 0u;
    ycc = (!info->hdmi2 || limited) ? 0u : 1u;
    k_memset(buf, 0, 4 + 13);
    buf[0] = 0x82; /* AVI */
    buf[1] = 2;
    buf[2] = 13;
    /* RGB, active format present, underscan. */
    buf[4] = (uint8_t)(1u << 4 | 2u);
    /* No colorimetry, the picture aspect, active aspect as the picture. */
    buf[5] = (uint8_t)((aspect & 3u) << 4 | 8u);
    buf[6] = (uint8_t)(quant << 2);
    buf[7] = (uint8_t)vic;
    /* Graphics, pixels sent once or twice. */
    buf[8] = (uint8_t)(ycc << 6 | ((mode->flags & DRM_MODE_FLAG_DBLCLK) ? 1u : 0u));
    infoframe_checksum(buf, 4 + 13);
    return 4 + 13;
}

uint32_t drm_hdmi_vendor_infoframe(const struct drm_edid_hdmi_info *info,
                                   const struct drm_mode_modeinfo *mode, uint8_t *buf)
{
    uint32_t vic, len;

    if (!info->has_hdmi_infoframe) {
        return 0;
    }
    vic = mode_hdmi_vic(info, mode);
    len = vic ? 5u : 4u;
    k_memset(buf, 0, 4 + len);
    buf[0] = 0x81; /* vendor */
    buf[1] = 1;
    buf[2] = (uint8_t)len;
    buf[4] = 0x03;
    buf[5] = 0x0c;
    buf[6] = 0x00;
    if (vic) {
        buf[7] = 1u << 5; /* an extended resolution */
        buf[8] = (uint8_t)vic;
    }
    infoframe_checksum(buf, 4 + len);
    return 4 + len;
}

uint32_t drm_hdmi_spd_infoframe(const char *vendor, const char *product, uint8_t sdi, uint8_t *buf)
{
    uint32_t i;

    k_memset(buf, 0, 4 + 25);
    buf[0] = 0x83; /* source product description */
    buf[1] = 1;
    buf[2] = 25;
    for (i = 0; i < 8 && vendor[i]; i++) {
        buf[4 + i] = (uint8_t)vendor[i];
    }
    for (i = 0; i < 16 && product[i]; i++) {
        buf[12 + i] = (uint8_t)product[i];
    }
    buf[28] = sdi;
    infoframe_checksum(buf, 4 + 25);
    return 4 + 25;
}

int32_t drm_edid_override_set(struct drm_connector *connector, const void *edid, uint32_t len)
{
    const uint8_t *bytes = (const uint8_t *)edid;
    uint8_t block[EDID_LENGTH];
    uint32_t blocks, i;
    uint8_t *copy;

    /* Linux's drm_edid_valid: every block the base names there and valid,
     * judged on a copy, since the check mends a header in place. */
    if (!edid || len < EDID_LENGTH) {
        return -EINVAL;
    }
    blocks = 1u + bytes[0x7e];
    if (blocks * EDID_LENGTH > len) {
        return -EINVAL;
    }
    for (i = 0; i < blocks; i++) {
        k_memcpy(block, bytes + i * EDID_LENGTH, EDID_LENGTH);
        if (!block_valid(block, i == 0)) {
            return -EINVAL;
        }
    }
    copy = (uint8_t *)kmalloc(len);
    if (!copy) {
        return -ENOMEM;
    }
    k_memcpy(copy, bytes, len);
    kfree(connector->edid_override);
    connector->edid_override = copy;
    connector->edid_override_len = len;
    return 0;
}

void drm_edid_override_reset(struct drm_connector *connector)
{
    kfree(connector->edid_override);
    connector->edid_override = NULL;
    connector->edid_override_len = 0;
}

int drm_add_modes_noedid(struct drm_connector *connector, uint32_t hdisplay, uint32_t vdisplay)
{
    struct drm_mode_modeinfo m;
    int i, n = 0;

    for (i = 0; i < NELEM(dmt_modes); i++) {
        if (hdisplay && vdisplay &&
            (dmt_modes[i].hdisplay > hdisplay || dmt_modes[i].vdisplay > vdisplay)) {
            continue;
        }
        mode_from_table(&m, &dmt_modes[i]);
        if (drm_mode_vrefresh(&m) > 61) {
            continue;
        }
        if (drm_mode_probed_add(connector, &m) == 0) {
            n++;
        }
    }
    return n;
}
