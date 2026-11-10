/*
 * intel_ddi.c: DDI A, the Haswell port that carries the eDP panel, after
 * Linux's display/intel_ddi.c: its buffer's translations and signal levels,
 * the DisplayPort transport's training patterns, and the port's and the
 * eDP transcoder's part of a mode set.
 */

#include "i915.h"
#include "i915_reg.h"
#include "intel_dp.h"

#include "../../klog.h"
#include "../../lib/kstring.h"

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

/* ------------------------------------------------------------------------
 * The mode set of the eDP pipe
 */

/* How long the pipe takes to stop once its transcoder is disabled, at
 * most (Linux's intel_wait_for_pipe_off). */
#define PIPE_OFF_US 100000u

/** The clock select of a link of `port_clock` kHz, Linux's
 * hsw_ddi_lcpll_get_dpll: the LCPLL output of half its symbol clock. */
static uint32_t lcpll_sel(uint32_t port_clock)
{
    switch (port_clock / 2u) {
    case 81000u:
        return PORT_CLK_SEL_LCPLL_810;
    case 135000u:
        return PORT_CLK_SEL_LCPLL_1350;
    default:
        return PORT_CLK_SEL_LCPLL_2700;
    }
}

/** The field of `bpp` bits a pixel in TRANS_DDI_FUNC_CTL. */
static uint32_t func_bpc(uint32_t bpp)
{
    switch (bpp) {
    case 24u:
        return TRANS_DDI_BPC_8;
    case 30u:
        return TRANS_DDI_BPC_10;
    case 36u:
        return TRANS_DDI_BPC_12;
    default:
        return TRANS_DDI_BPC_6;
    }
}

/** The eDP transcoder's main stream attributes, Linux's
 * intel_ddi_set_dp_msa: clocks synchronous and the colour depth. */
static void set_dp_msa(struct drm_i915_private *i915, const struct intel_crtc_config *cfg)
{
    uint32_t bpc = cfg->bpp / 3u;

    i915_write(&i915->hw, TRANS_MSA_MISC(I915_TRANS_EDP),
               TRANS_MSA_SYNC_CLK | ((bpc - 6u) / 2u) << TRANS_MSA_BPC_SHIFT);
}

/** Turn the transcoder's infoframes off, Linux's intel_dp_set_infoframes
 * for a stream that sends none, as eDP without PSR does whether it starts
 * or stops. */
static void clear_infoframes(struct drm_i915_private *i915)
{
    uint32_t reg = HSW_TVIDEO_DIP_CTL(I915_TRANS_EDP);
    uint32_t dip = VIDEO_DIP_ENABLE_AVI_HSW | VIDEO_DIP_ENABLE_GCP_HSW | VIDEO_DIP_ENABLE_VS_HSW |
                   VIDEO_DIP_ENABLE_GMP_HSW | VIDEO_DIP_ENABLE_SPD_HSW | VIDEO_DIP_ENABLE_DRM_GLK |
                   VIDEO_DIP_ENABLE_VSC_HSW;

    i915_write(&i915->hw, reg, i915_read(&i915->hw, reg) & ~dip);
    i915_posting_read(&i915->hw, reg);
}

int32_t intel_ddi_pre_enable(struct drm_i915_private *i915, const struct intel_crtc_config *cfg)
{
    struct intel_dp *dp = &i915->display.dp;
    int32_t err;

    /* Linux's intel_dp_set_link_params. */
    k_memset(dp->train_set, 0, sizeof(dp->train_set));
    dp->link_trained = 0;
    dp->link_rate = cfg->port_clock;
    dp->lane_count = cfg->lanes;
    intel_ddi_init_dp_buf_reg(i915, cfg->lanes);
    intel_pps_on(i915);
    i915_write(&i915->hw, PORT_CLK_SEL(dp->port), lcpll_sel(cfg->port_clock));
    intel_ddi_prepare_buf_trans(i915);
    intel_ddi_set_signal_levels(i915);
    intel_dp_set_power(i915, DP_SET_POWER_D0);
    err = intel_dp_start_link_train(i915, cfg->mode.clock);
    /* Port A stops its training once the pipe runs (intel_ddi_enable). */
    set_dp_msa(i915, cfg);
    return err;
}

void intel_ddi_enable(struct drm_i915_private *i915, int pipe, const struct intel_crtc_config *cfg)
{
    struct i915_hw *hw = &i915->hw;
    uint32_t ctl, conf;

    /* Linux's intel_ddi_transcoder_func_reg_val_get. */
    ctl = TRANS_DDI_FUNC_ENABLE | TRANS_DDI_SELECT_PORT(i915->display.dp.port) |
          func_bpc(cfg->bpp) | TRANS_DDI_MODE_SELECT_DP_SST | DDI_PORT_WIDTH(cfg->lanes);
    if (cfg->mode.flags & DRM_MODE_FLAG_PVSYNC) {
        ctl |= TRANS_DDI_PVSYNC;
    }
    if (cfg->mode.flags & DRM_MODE_FLAG_PHSYNC) {
        ctl |= TRANS_DDI_PHSYNC;
    }
    /* Pipe A may feed eDP from the always-on power well only while the
     * fitter is unused. */
    ctl |= pipe == I915_PIPE_B   ? TRANS_DDI_EDP_INPUT_B_ONOFF
           : pipe == I915_PIPE_C ? TRANS_DDI_EDP_INPUT_C_ONOFF
           : cfg->force_thru     ? TRANS_DDI_EDP_INPUT_A_ONOFF
                                 : TRANS_DDI_EDP_INPUT_A_ON;
    i915_write(hw, TRANS_DDI_FUNC_CTL(I915_TRANS_EDP), ctl);
    /* Linux's intel_enable_transcoder. */
    conf = i915_read(hw, TRANSCONF(I915_TRANS_EDP));
    if (!(conf & TRANSCONF_ENABLE)) {
        i915_write(hw, TRANSCONF(I915_TRANS_EDP), conf | TRANSCONF_ENABLE);
        i915_posting_read(hw, TRANSCONF(I915_TRANS_EDP));
    }
    /* Linux's intel_enable_ddi_dp: port A before Gen 9 leaves its
     * training pattern only now. */
    intel_dp_stop_link_train(i915);
    intel_backlight_enable(i915);
    intel_pps_backlight_on(i915);
    clear_infoframes(i915);
}

void intel_ddi_disable(struct drm_i915_private *i915)
{
    i915->display.dp.link_trained = 0;
    intel_pps_backlight_off(i915);
    intel_backlight_disable(i915);
}

void intel_ddi_post_disable(struct drm_i915_private *i915, struct intel_crtc *crtc,
                            const struct intel_crtc_config *cfg)
{
    struct intel_dp *dp = &i915->display.dp;
    struct i915_hw *hw = &i915->hw;
    uint32_t conf, ctl, buf;
    int wait = 0;

    drm_crtc_vblank_off(&crtc->base);
    /* Linux's intel_disable_transcoder. */
    conf = i915_read(hw, TRANSCONF(I915_TRANS_EDP));
    if (conf & TRANSCONF_ENABLE) {
        i915_write(hw, TRANSCONF(I915_TRANS_EDP), conf & ~TRANSCONF_ENABLE);
        if (i915_wait_for(hw, "pipe off", TRANSCONF(I915_TRANS_EDP), TRANSCONF_STATE_ENABLE, 0,
                          PIPE_OFF_US) != 0) {
            klog(KLOG_ERR, "i915", "ddi: pipe %c off wait timed out", 'A' + crtc->pipe);
        }
    }
    /* Linux's intel_ddi_disable_transcoder_func. */
    ctl = i915_read(hw, TRANS_DDI_FUNC_CTL(I915_TRANS_EDP));
    ctl &= ~(TRANS_DDI_FUNC_ENABLE | TRANS_DDI_PORT_MASK | TRANS_DDI_MODE_SELECT_MASK);
    i915_write(hw, TRANS_DDI_FUNC_CTL(I915_TRANS_EDP), ctl);
    ilk_pfit_disable(i915, crtc->pipe, cfg);
    /* Linux's intel_ddi_post_disable_dp: the sink powered down before the
     * port, lest it signal the link's loss. */
    clear_infoframes(i915);
    intel_dp_set_power(i915, DP_SET_POWER_D3);
    buf = i915_read(hw, DDI_BUF_CTL(dp->port));
    if (buf & DDI_BUF_CTL_ENABLE) {
        i915_write(hw, DDI_BUF_CTL(dp->port), buf & ~DDI_BUF_CTL_ENABLE);
        wait = 1;
    }
    dp->buf_ctl &= ~DDI_BUF_CTL_ENABLE;
    i915_rmw(hw, DP_TP_CTL(dp->port), DP_TP_CTL_ENABLE, 0);
    if (wait) {
        intel_ddi_wait_buf_idle(i915);
    }
    (void)intel_pps_vdd_on_unlocked(i915);
    intel_pps_off(i915);
    i915_write(hw, PORT_CLK_SEL(dp->port), PORT_CLK_SEL_NONE);
}
