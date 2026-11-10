/*
 * intel_ddi.c: DDI A, the Haswell port that carries the eDP panel, after
 * Linux's display/intel_ddi.c: its buffer's translations and signal levels,
 * and the DisplayPort transport's training patterns.
 */

#include "i915.h"
#include "i915_reg.h"
#include "intel_dp.h"

#include "../../klog.h"

/* How long DDI_BUF_CTL takes to drive the lanes once enabled (Linux waits
 * more than 518 us before Gen 10), and the most it takes to go idle. */
#define DDI_BUF_ACTIVE_US 518u
#define DDI_BUF_IDLE_US   8u

/* Linux's hsw_trans_dp: each DDI_BUF_TRANS_SELECT's two words, for
 * DisplayPort and eDP alike on Haswell. */
static const uint32_t hsw_trans_dp[INTEL_DDI_DP_LEVELS][2] = {
    {0x00ffffffu, 0x0006000eu}, {0x00d75fffu, 0x0005000au}, {0x00c30fffu, 0x00040006u},
    {0x80aaafffu, 0x000b0000u}, {0x00ffffffu, 0x0005000au}, {0x00d75fffu, 0x000c0004u},
    {0x80c30fffu, 0x000b0000u}, {0x00ffffffu, 0x00040006u}, {0x80d75fffu, 0x000b0000u}};

/* Linux's index_to_dp_signal_levels: the swing and pre-emphasis that each
 * translation gives, of the nine Haswell has. */
static const uint8_t dp_signal_levels[INTEL_DDI_DP_LEVELS] = {
    DP_TRAIN_VOLTAGE_SWING_LEVEL_0 | DP_TRAIN_PRE_EMPH_LEVEL_0,
    DP_TRAIN_VOLTAGE_SWING_LEVEL_0 | DP_TRAIN_PRE_EMPH_LEVEL_1,
    DP_TRAIN_VOLTAGE_SWING_LEVEL_0 | DP_TRAIN_PRE_EMPH_LEVEL_2,
    DP_TRAIN_VOLTAGE_SWING_LEVEL_0 | DP_TRAIN_PRE_EMPH_LEVEL_3,
    DP_TRAIN_VOLTAGE_SWING_LEVEL_1 | DP_TRAIN_PRE_EMPH_LEVEL_0,
    DP_TRAIN_VOLTAGE_SWING_LEVEL_1 | DP_TRAIN_PRE_EMPH_LEVEL_1,
    DP_TRAIN_VOLTAGE_SWING_LEVEL_1 | DP_TRAIN_PRE_EMPH_LEVEL_2,
    DP_TRAIN_VOLTAGE_SWING_LEVEL_2 | DP_TRAIN_PRE_EMPH_LEVEL_0,
    DP_TRAIN_VOLTAGE_SWING_LEVEL_2 | DP_TRAIN_PRE_EMPH_LEVEL_1};

void intel_ddi_prepare_buf_trans(struct drm_i915_private *i915)
{
    int port = i915->display.dp.port, i;

    for (i = 0; i < INTEL_DDI_DP_LEVELS; i++) {
        i915_write(&i915->hw, DDI_BUF_TRANS_LO(port, i), hsw_trans_dp[i][0]);
        i915_write(&i915->hw, DDI_BUF_TRANS_HI(port, i), hsw_trans_dp[i][1]);
    }
}

uint8_t intel_ddi_dp_voltage_max(void)
{
    return dp_signal_levels[INTEL_DDI_DP_LEVELS - 1] & DP_TRAIN_VOLTAGE_SWING_MASK;
}

void intel_ddi_init_dp_buf_reg(struct drm_i915_private *i915, uint32_t lanes)
{
    struct intel_dp *dp = &i915->display.dp;

    dp->buf_ctl = dp->saved_port_bits | DDI_PORT_WIDTH(lanes) | DDI_BUF_TRANS_SELECT(0);
}

/** The translation that gives lane 0's levels, Linux's
 * translate_signal_level; 0 for levels Haswell has none of. */
static int dp_level(struct drm_i915_private *i915)
{
    uint8_t want =
        i915->display.dp.train_set[0] & (DP_TRAIN_VOLTAGE_SWING_MASK | DP_TRAIN_PRE_EMPHASIS_MASK);
    int i;

    for (i = 0; i < INTEL_DDI_DP_LEVELS; i++) {
        if (dp_signal_levels[i] == want) {
            return i;
        }
    }
    klog(KLOG_WARN, "i915", "ddi: unsupported swing and pre-emphasis 0x%x", (unsigned)want);
    return 0;
}

void intel_ddi_set_signal_levels(struct drm_i915_private *i915)
{
    struct intel_dp *dp = &i915->display.dp;

    dp->buf_ctl = (dp->buf_ctl & ~DDI_BUF_EMP_MASK) | DDI_BUF_TRANS_SELECT(dp_level(i915));
    i915_write(&i915->hw, DDI_BUF_CTL(dp->port), dp->buf_ctl);
    i915_posting_read(&i915->hw, DDI_BUF_CTL(dp->port));
}

void intel_ddi_wait_buf_idle(struct drm_i915_private *i915)
{
    int port = i915->display.dp.port;

    if (i915_wait_for(&i915->hw, "ddi buf idle", DDI_BUF_CTL(port), DDI_BUF_IS_IDLE,
                      DDI_BUF_IS_IDLE, DDI_BUF_IDLE_US) != 0) {
        klog(KLOG_ERR, "i915", "ddi: timeout waiting for DDI BUF %c to get idle", 'A' + port);
    }
}

void intel_ddi_prepare_link_retrain(struct drm_i915_private *i915, int enhanced)
{
    struct intel_dp *dp = &i915->display.dp;
    struct i915_hw *hw = &i915->hw;
    uint32_t tp = i915_read(hw, DP_TP_CTL(dp->port)), buf;
    int wait = 0;

    if (tp & DP_TP_CTL_ENABLE) {
        buf = i915_read(hw, DDI_BUF_CTL(dp->port));
        if (buf & DDI_BUF_CTL_ENABLE) {
            i915_write(hw, DDI_BUF_CTL(dp->port), buf & ~DDI_BUF_CTL_ENABLE);
            wait = 1;
        }
        i915_write(hw, DP_TP_CTL(dp->port), tp & ~DP_TP_CTL_ENABLE);
        i915_posting_read(hw, DP_TP_CTL(dp->port));
        if (wait) {
            intel_ddi_wait_buf_idle(i915);
        }
    }
    tp = DP_TP_CTL_ENABLE | DP_TP_CTL_LINK_TRAIN_PAT1 | DP_TP_CTL_MODE_SST;
    if (enhanced) {
        tp |= DP_TP_CTL_ENHANCED_FRAME_ENABLE;
    }
    i915_write(hw, DP_TP_CTL(dp->port), tp);
    i915_posting_read(hw, DP_TP_CTL(dp->port));
    dp->buf_ctl |= DDI_BUF_CTL_ENABLE;
    i915_write(hw, DDI_BUF_CTL(dp->port), dp->buf_ctl);
    i915_posting_read(hw, DDI_BUF_CTL(dp->port));
    i915_delay_us(hw, DDI_BUF_ACTIVE_US);
}

void intel_ddi_set_link_train(struct drm_i915_private *i915, uint8_t dp_train_pat)
{
    int port = i915->display.dp.port;
    uint32_t tp = i915_read(&i915->hw, DP_TP_CTL(port)) & ~DP_TP_CTL_LINK_TRAIN_MASK;

    switch (dp_train_pat & DP_TRAINING_PATTERN_MASK) {
    case DP_TRAINING_PATTERN_1:
        tp |= DP_TP_CTL_LINK_TRAIN_PAT1;
        break;
    case DP_TRAINING_PATTERN_2:
        tp |= DP_TP_CTL_LINK_TRAIN_PAT2;
        break;
    case DP_TRAINING_PATTERN_3:
        tp |= DP_TP_CTL_LINK_TRAIN_PAT3;
        break;
    default:
        tp |= DP_TP_CTL_LINK_TRAIN_NORMAL;
        break;
    }
    i915_write(&i915->hw, DP_TP_CTL(port), tp);
}

void intel_ddi_set_idle_link_train(struct drm_i915_private *i915)
{
    int port = i915->display.dp.port;

    i915_rmw(&i915->hw, DP_TP_CTL(port), DP_TP_CTL_LINK_TRAIN_MASK, DP_TP_CTL_LINK_TRAIN_IDLE);
    /* Port A carries only eDP in SST, where the idle patterns need no
     * wait before Gen 12 (Linux's intel_ddi_set_idle_link_train). */
    if (port == 0) {
        return;
    }
    if (i915_wait_for(&i915->hw, "dp idle patterns", DP_TP_STATUS(port), DP_TP_STATUS_IDLE_DONE,
                      DP_TP_STATUS_IDLE_DONE, 2000u) != 0) {
        klog(KLOG_ERR, "i915", "ddi: timed out waiting for DP idle patterns");
    }
}
