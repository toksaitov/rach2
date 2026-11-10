/*
 * intel_dp.h: the DisplayPort sink's registers, its DPCD, and the AUX
 * channel's requests and replies, as Linux's include/drm/display/drm_dp.h
 * names them, for intel_dp.c and the files beside it.
 */
#ifndef _INTEL_DP_H_
#define _INTEL_DP_H_

/* ---- The receiver's capabilities, DPCD 0x000 to 0x00e ---- */

#define DP_DPCD_REV                            0x000
#define DP_DPCD_REV_11                         0x11
#define DP_DPCD_REV_14                         0x14
#define DP_MAX_LINK_RATE                       0x001
#define DP_MAX_LANE_COUNT                      0x002
#define DP_MAX_LANE_COUNT_MASK                 0x1f
#define DP_TPS3_SUPPORTED                      (1 << 6)
#define DP_ENHANCED_FRAME_CAP                  (1 << 7)
#define DP_MAX_DOWNSPREAD                      0x003
#define DP_MAX_DOWNSPREAD_0_5                  (1 << 0)
#define DP_TPS4_SUPPORTED                      (1 << 7)
#define DP_DOWNSTREAMPORT_PRESENT              0x005
#define DP_MAIN_LINK_CHANNEL_CODING            0x006
#define DP_CAP_ANSI_8B10B                      (1 << 0)
#define DP_DOWN_STREAM_PORT_COUNT              0x007
#define DP_OUI_SUPPORT                         (1 << 7)
#define DP_EDP_CONFIGURATION_CAP               0x00d
#define DP_TRAINING_AUX_RD_INTERVAL            0x00e
#define DP_TRAINING_AUX_RD_MASK                0x7f
#define DP_EXTENDED_RECEIVER_CAP_FIELD_PRESENT (1 << 7)
#define DP_RECEIVER_CAP_SIZE                   0xf
#define DP_SUPPORTED_LINK_RATES                0x010 /* eDP 1.4: 8 rates of 200 kHz */
#define DP_MAX_SUPPORTED_RATES                 8

/* The link rates DP_MAX_LINK_RATE and DP_LINK_BW_SET name, in 0.27 Gb/s. */
#define DP_LINK_BW_1_62 0x06
#define DP_LINK_BW_2_7  0x0a
#define DP_LINK_BW_5_4  0x14

/* ---- The link's configuration, DPCD 0x100 to 0x115 ---- */

#define DP_LINK_BW_SET                    0x100
#define DP_LANE_COUNT_SET                 0x101
#define DP_LANE_COUNT_ENHANCED_FRAME_EN   (1 << 7)
#define DP_TRAINING_PATTERN_SET           0x102
#define DP_TRAINING_PATTERN_DISABLE       0
#define DP_TRAINING_PATTERN_1             1
#define DP_TRAINING_PATTERN_2             2
#define DP_TRAINING_PATTERN_3             3
#define DP_TRAINING_PATTERN_MASK          0x3
#define DP_LINK_SCRAMBLING_DISABLE        (1 << 5)
#define DP_TRAINING_LANE0_SET             0x103
#define DP_TRAIN_VOLTAGE_SWING_MASK       0x3
#define DP_TRAIN_MAX_SWING_REACHED        (1 << 2)
#define DP_TRAIN_PRE_EMPHASIS_SHIFT       3
#define DP_TRAIN_PRE_EMPHASIS_MASK        (3 << 3)
#define DP_TRAIN_MAX_PRE_EMPHASIS_REACHED (1 << 5)
#define DP_TRAIN_VOLTAGE_SWING_LEVEL_2    2
#define DP_TRAIN_VOLTAGE_SWING_LEVEL_3    3
#define DP_TRAIN_PRE_EMPH_LEVEL_3         (3 << 3)
#define DP_DOWNSPREAD_CTRL                0x107
#define DP_MAIN_LINK_CHANNEL_CODING_SET   0x108
#define DP_SET_ANSI_8B10B                 (1 << 0)
#define DP_LINK_RATE_SET                  0x115

/* ---- The link's status, DPCD 0x202 to 0x207 ---- */

#define DP_LANE0_1_STATUS            0x202
#define DP_LANE_CR_DONE              (1 << 0)
#define DP_LANE_CHANNEL_EQ_DONE      (1 << 1)
#define DP_LANE_SYMBOL_LOCKED        (1 << 2)
#define DP_LANE_ALIGN_STATUS_UPDATED 0x204
#define DP_INTERLANE_ALIGN_DONE      (1 << 0)
#define DP_ADJUST_REQUEST_LANE0_1    0x206
#define DP_LINK_STATUS_SIZE          6

/* ---- The sink's power, the source's OUI, eDP's capabilities ---- */

#define DP_SOURCE_OUI            0x300
#define DP_SET_POWER             0x600
#define DP_SET_POWER_D0          0x1
#define DP_SET_POWER_D3          0x2
#define DP_EDP_DPCD_REV          0x700
#define DP_EDP_14                0x03
#define EDP_DISPLAY_CTL_CAP_SIZE 3
#define DP_DP13_DPCD_REV         0x2200 /* the extended receiver capabilities */

/* ---- The AUX channel's requests and replies ---- */

#define DP_AUX_I2C_WRITE               0x0
#define DP_AUX_I2C_READ                0x1
#define DP_AUX_I2C_WRITE_STATUS_UPDATE 0x2
#define DP_AUX_I2C_MOT                 0x4
#define DP_AUX_NATIVE_WRITE            0x8
#define DP_AUX_NATIVE_READ             0x9
#define DP_AUX_NATIVE_REPLY_ACK        (0x0 << 0)
#define DP_AUX_NATIVE_REPLY_NACK       (0x1 << 0)
#define DP_AUX_NATIVE_REPLY_DEFER      (0x2 << 0)
#define DP_AUX_NATIVE_REPLY_MASK       (0x3 << 0)
#define DP_AUX_I2C_REPLY_ACK           (0x0 << 2)
#define DP_AUX_I2C_REPLY_NACK          (0x1 << 2)
#define DP_AUX_I2C_REPLY_DEFER         (0x2 << 2)
#define DP_AUX_I2C_REPLY_MASK          (0x3 << 2)
#define DP_AUX_MAX_PAYLOAD_BYTES       16

/* The EDID's address on the sink's I2C bus, and its segment pointer's. */
#define DDC_ADDR         0x50
#define DDC_SEGMENT_ADDR 0x30

#endif /* _INTEL_DP_H_ */
