/*
 * intel_ddi.c: Haswell's DDI ports, A with the eDP panel and B to D with
 * monitors, after Linux's display/intel_ddi.c: their buffers' translations
 * and signal levels, the DisplayPort transport's training patterns, and
 * the port's and its transcoder's part of a mode set for eDP, DisplayPort
 * and HDMI.
 */

#include "i915.h"
#include "i915_reg.h"
#include "intel_dp.h"

#include "../../klog.h"
#include "../../lib/kstring.h"

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

/* Linux's hsw_trans_hdmi: the twelve translations an HDMI port may take as
 * its entry 9, the VBT's level choosing, 6 by default. */
#define HDMI_LEVELS        12
#define HDMI_DEFAULT_LEVEL 6
static const uint32_t hsw_trans_hdmi[HDMI_LEVELS][2] = {
    {0x00ffffffu, 0x0006000eu}, {0x00e79fffu, 0x000e000cu}, {0x00d75fffu, 0x0005000au},
    {0x00ffffffu, 0x0005000au}, {0x00e79fffu, 0x001d0007u}, {0x00d75fffu, 0x000c0004u},
    {0x00ffffffu, 0x00040006u}, {0x80e79fffu, 0x00030002u}, {0x00ffffffu, 0x00140005u},
    {0x00ffffffu, 0x000c0004u}, {0x00ffffffu, 0x001c0003u}, {0x80ffffffu, 0x00030002u}};

void intel_ddi_prepare_buf_trans(struct intel_dp *dp)
{
    int i;

    for (i = 0; i < INTEL_DDI_DP_LEVELS; i++) {
        i915_write(&dp->i915->hw, DDI_BUF_TRANS_LO(dp->port, i), hsw_trans_dp[i][0]);
        i915_write(&dp->i915->hw, DDI_BUF_TRANS_HI(dp->port, i), hsw_trans_dp[i][1]);
    }
}

int intel_ddi_hdmi_default_level(void)
{
    return HDMI_DEFAULT_LEVEL;
}

void intel_ddi_prepare_hdmi_buf_trans(struct drm_i915_private *i915, int port, int level)
{
    if (level < 0 || level >= HDMI_LEVELS) {
        level = level < 0 ? HDMI_DEFAULT_LEVEL : HDMI_LEVELS - 1;
    }
    i915_write(&i915->hw, DDI_BUF_TRANS_LO(port, 9), hsw_trans_hdmi[level][0]);
    i915_write(&i915->hw, DDI_BUF_TRANS_HI(port, 9), hsw_trans_hdmi[level][1]);
}

uint8_t intel_ddi_dp_voltage_max(void)
{
    return dp_signal_levels[INTEL_DDI_DP_LEVELS - 1] & DP_TRAIN_VOLTAGE_SWING_MASK;
}

void intel_ddi_init_dp_buf_reg(struct intel_dp *dp, uint32_t lanes)
{
    dp->buf_ctl = dp->saved_port_bits | DDI_PORT_WIDTH(lanes) | DDI_BUF_TRANS_SELECT(0);
}

/** The translation that gives lane 0's levels, Linux's
 * translate_signal_level; 0 for levels Haswell has none of. */
static int dp_level(const struct intel_dp *dp)
{
    uint8_t want = dp->train_set[0] & (DP_TRAIN_VOLTAGE_SWING_MASK | DP_TRAIN_PRE_EMPHASIS_MASK);
    int i;

    for (i = 0; i < INTEL_DDI_DP_LEVELS; i++) {
        if (dp_signal_levels[i] == want) {
            return i;
        }
    }
    klog(KLOG_WARN, "i915", "%s: unsupported swing and pre-emphasis 0x%x", dp->name,
         (unsigned)want);
    return 0;
}

void intel_ddi_set_signal_levels(struct intel_dp *dp)
{
    struct i915_hw *hw = &dp->i915->hw;

    dp->buf_ctl = (dp->buf_ctl & ~DDI_BUF_EMP_MASK) | DDI_BUF_TRANS_SELECT(dp_level(dp));
    i915_write(hw, DDI_BUF_CTL(dp->port), dp->buf_ctl);
    i915_posting_read(hw, DDI_BUF_CTL(dp->port));
}

/** Wait, 8 us at most, for port `port`'s buffer to go idle, Linux's
 * intel_wait_ddi_buf_idle; a timeout is logged. */
static void wait_buf_idle(struct drm_i915_private *i915, int port)
{
    if (i915_wait_for(&i915->hw, "ddi buf idle", DDI_BUF_CTL(port), DDI_BUF_IS_IDLE,
                      DDI_BUF_IS_IDLE, DDI_BUF_IDLE_US) != 0) {
        klog(KLOG_ERR, "i915", "ddi: timeout waiting for DDI BUF %c to get idle", 'A' + port);
    }
}

void intel_ddi_wait_buf_idle(struct intel_dp *dp)
{
    wait_buf_idle(dp->i915, dp->port);
}

void intel_ddi_prepare_link_retrain(struct intel_dp *dp, int enhanced)
{
    struct i915_hw *hw = &dp->i915->hw;
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
            intel_ddi_wait_buf_idle(dp);
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

void intel_ddi_set_link_train(struct intel_dp *dp, uint8_t dp_train_pat)
{
    struct i915_hw *hw = &dp->i915->hw;
    uint32_t tp = i915_read(hw, DP_TP_CTL(dp->port)) & ~DP_TP_CTL_LINK_TRAIN_MASK;

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
    i915_write(hw, DP_TP_CTL(dp->port), tp);
}

void intel_ddi_set_idle_link_train(struct intel_dp *dp)
{
    struct i915_hw *hw = &dp->i915->hw;

    i915_rmw(hw, DP_TP_CTL(dp->port), DP_TP_CTL_LINK_TRAIN_MASK, DP_TP_CTL_LINK_TRAIN_IDLE);
    /* Port A carries only eDP in SST, where the idle patterns need no
     * wait before Gen 12 (Linux's intel_ddi_set_idle_link_train). */
    if (dp->port == INTEL_PORT_A) {
        return;
    }
    if (i915_wait_for(hw, "dp idle patterns", DP_TP_STATUS(dp->port), DP_TP_STATUS_IDLE_DONE,
                      DP_TP_STATUS_IDLE_DONE, 2000u) != 0) {
        klog(KLOG_ERR, "i915", "%s: timed out waiting for DP idle patterns", dp->name);
    }
}

/* ------------------------------------------------------------------------
 * The mode set of a pipe on a port
 */

/** The DisplayPort sink of port `port`: the panel on A, a monitor's on B
 * to D. */
static struct intel_dp *port_dp(struct drm_i915_private *i915, int port)
{
    return port == INTEL_PORT_A ? &i915->display.dp : &i915->display.ddi[port - 1].dp;
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

/** The transcoder's main stream attributes, Linux's intel_ddi_set_dp_msa:
 * clocks synchronous, the colour depth, and CEA-861's limited range. */
static void set_dp_msa(struct drm_i915_private *i915, const struct intel_crtc_config *cfg)
{
    uint32_t bpc = cfg->bpp / 3u;

    i915_write(&i915->hw, TRANS_MSA_MISC(cfg->trans),
               TRANS_MSA_SYNC_CLK | ((bpc - 6u) / 2u) << TRANS_MSA_BPC_SHIFT |
                   (cfg->limited_range ? TRANS_MSA_CEA_RGB : 0));
}

/** Turn the transcoder's infoframes off, Linux's intel_dp_set_infoframes
 * for a stream that sends none, as DisplayPort without PSR does whether it
 * starts or stops. */
static void clear_infoframes(struct drm_i915_private *i915, int trans)
{
    uint32_t reg = HSW_TVIDEO_DIP_CTL(trans);
    uint32_t dip = VIDEO_DIP_ENABLE_AVI_HSW | VIDEO_DIP_ENABLE_GCP_HSW | VIDEO_DIP_ENABLE_VS_HSW |
                   VIDEO_DIP_ENABLE_GMP_HSW | VIDEO_DIP_ENABLE_SPD_HSW | VIDEO_DIP_ENABLE_DRM_GLK |
                   VIDEO_DIP_ENABLE_VSC_HSW;

    i915_write(&i915->hw, reg, i915_read(&i915->hw, reg) & ~dip);
    i915_posting_read(&i915->hw, reg);
}

/** Feed the transcoder the port's clock, Linux's
 * intel_ddi_enable_transcoder_clock; the eDP transcoder has its own. */
static void enable_transcoder_clock(struct drm_i915_private *i915,
                                    const struct intel_crtc_config *cfg)
{
    if (cfg->trans != I915_TRANS_EDP) {
        i915_write(&i915->hw, TRANS_CLK_SEL(cfg->trans), TRANS_CLK_SEL_PORT(cfg->port));
    }
}

/** Take the clock from the transcoder, Linux's
 * intel_ddi_disable_transcoder_clock. */
static void disable_transcoder_clock(struct drm_i915_private *i915,
                                     const struct intel_crtc_config *cfg)
{
    if (cfg->trans != I915_TRANS_EDP) {
        i915_write(&i915->hw, TRANS_CLK_SEL(cfg->trans), TRANS_CLK_SEL_DISABLED);
    }
}

/** Tell a DP 1.3 branch whether its sink is HDMI and that it converts
 * nothing, Linux's intel_dp_configure_protocol_converter for RGB. */
static void configure_protocol_converter(struct intel_dp *dp)
{
    uint8_t zero[2] = {0, 0};

    if (dp->dpcd[DP_DPCD_REV] < 0x13 || !(dp->dpcd[DP_DOWNSTREAMPORT_PRESENT] & 1u)) {
        return;
    }
    if (intel_dp_dpcd_writeb(dp, 0x3050, dp->info.is_hdmi ? 1u : 0u) != 1) {
        klog(KLOG_DEBUG, "i915", "%s: failed to set the protocol converter's HDMI mode", dp->name);
    }
    (void)intel_dp_dpcd_write(dp, 0x3051, zero, 2);
}

/** Bring a DisplayPort port up, Linux's hsw_ddi_pre_enable_dp with
 * intel_ddi_set_dp_msa: the panel's power, the port's clock and buffer,
 * the sink in D0, the link trained, the transcoder's clock and the stream's
 * attributes. 0, or `intel_dp_start_link_train`'s error. */
static int32_t pre_enable_dp(struct drm_i915_private *i915, const struct intel_crtc_config *cfg)
{
    struct intel_dp *dp = port_dp(i915, cfg->port);
    int32_t err;

    /* Linux's intel_dp_set_link_params. */
    k_memset(dp->train_set, 0, sizeof(dp->train_set));
    dp->link_trained = 0;
    dp->link_rate = cfg->port_clock;
    dp->lane_count = cfg->lanes;
    intel_ddi_init_dp_buf_reg(dp, cfg->lanes);
    if (dp->edp) {
        intel_pps_on(i915);
    }
    i915_write(&i915->hw, PORT_CLK_SEL(dp->port), intel_dpll_port_clk_sel(cfg->pll));
    intel_ddi_prepare_buf_trans(dp);
    intel_ddi_set_signal_levels(dp);
    intel_dp_set_power(dp, DP_SET_POWER_D0);
    configure_protocol_converter(dp);
    err = intel_dp_start_link_train(dp, cfg->mode.clock);
    /* Port A stops its training once the pipe runs (intel_ddi_enable). */
    if (dp->port != INTEL_PORT_A) {
        intel_dp_stop_link_train(dp);
    }
    enable_transcoder_clock(i915, cfg);
    set_dp_msa(i915, cfg);
    return err;
}

int32_t intel_ddi_pre_enable(struct drm_i915_private *i915, const struct intel_crtc_config *cfg)
{
    struct intel_ddi_port *p;

    if (cfg->output != INTEL_OUTPUT_HDMI) {
        return pre_enable_dp(i915, cfg);
    }
    /* Linux's intel_ddi_pre_enable_hdmi. */
    p = &i915->display.ddi[cfg->port - 1];
    intel_hdmi_dual_mode_tmds(p, 1);
    i915_write(&i915->hw, PORT_CLK_SEL(cfg->port), intel_dpll_port_clk_sel(cfg->pll));
    enable_transcoder_clock(i915, cfg);
    intel_hdmi_set_infoframes(i915, 1, cfg);
    return 0;
}

void intel_ddi_enable(struct drm_i915_private *i915, int pipe, const struct intel_crtc_config *cfg)
{
    struct i915_hw *hw = &i915->hw;
    struct intel_dp *dp;
    uint32_t ctl, conf;

    /* Linux's intel_ddi_transcoder_func_reg_val_get. */
    ctl = TRANS_DDI_FUNC_ENABLE | TRANS_DDI_SELECT_PORT(cfg->port) | func_bpc(cfg->bpp);
    if (cfg->mode.flags & DRM_MODE_FLAG_PVSYNC) {
        ctl |= TRANS_DDI_PVSYNC;
    }
    if (cfg->mode.flags & DRM_MODE_FLAG_PHSYNC) {
        ctl |= TRANS_DDI_PHSYNC;
    }
    /* Pipe A may feed eDP from the always-on power well only while the
     * fitter is unused. */
    if (cfg->trans == I915_TRANS_EDP) {
        ctl |= pipe == I915_PIPE_B   ? TRANS_DDI_EDP_INPUT_B_ONOFF
               : pipe == I915_PIPE_C ? TRANS_DDI_EDP_INPUT_C_ONOFF
               : cfg->force_thru     ? TRANS_DDI_EDP_INPUT_A_ONOFF
                                     : TRANS_DDI_EDP_INPUT_A_ON;
    }
    if (cfg->output == INTEL_OUTPUT_HDMI) {
        ctl |= cfg->has_hdmi_sink ? TRANS_DDI_MODE_SELECT_HDMI : TRANS_DDI_MODE_SELECT_DVI;
    } else {
        ctl |= TRANS_DDI_MODE_SELECT_DP_SST | DDI_PORT_WIDTH(cfg->lanes);
    }
    i915_write(hw, TRANS_DDI_FUNC_CTL(cfg->trans), ctl);
    /* Linux's intel_enable_transcoder. */
    conf = i915_read(hw, TRANSCONF(cfg->trans));
    if (!(conf & TRANSCONF_ENABLE)) {
        i915_write(hw, TRANSCONF(cfg->trans), conf | TRANSCONF_ENABLE);
        i915_posting_read(hw, TRANSCONF(cfg->trans));
    }
    if (cfg->output == INTEL_OUTPUT_HDMI) {
        /* Linux's intel_enable_ddi_hdmi: the VBT's translation as entry 9,
         * then the buffer on; HDMI takes neither width nor levels. */
        intel_ddi_prepare_hdmi_buf_trans(i915, cfg->port,
                                         i915->display.ddi[cfg->port - 1].hdmi.level);
        i915_write(hw, DDI_BUF_CTL(cfg->port),
                   port_dp(i915, cfg->port)->saved_port_bits | DDI_BUF_CTL_ENABLE);
        i915_delay_us(hw, DDI_BUF_ACTIVE_US);
        return;
    }
    /* Linux's intel_enable_ddi_dp: port A before Gen 9 leaves its
     * training pattern only now. */
    dp = port_dp(i915, cfg->port);
    if (dp->port == INTEL_PORT_A) {
        intel_dp_stop_link_train(dp);
    }
    if (dp->edp) {
        intel_backlight_enable(i915);
        intel_pps_backlight_on(i915);
    }
    clear_infoframes(i915, cfg->trans);
}

void intel_ddi_disable(struct drm_i915_private *i915, const struct intel_crtc_config *cfg)
{
    struct intel_dp *dp;

    if (cfg->output == INTEL_OUTPUT_HDMI) {
        return;
    }
    dp = port_dp(i915, cfg->port);
    dp->link_trained = 0;
    if (dp->edp) {
        intel_pps_backlight_off(i915);
        intel_backlight_disable(i915);
    }
}

/** Turn port `port`'s buffer off, and its transport for DisplayPort, then
 * wait for it to go idle, Linux's intel_disable_ddi_buf. */
static void disable_buf(struct drm_i915_private *i915, int port, int dp)
{
    struct i915_hw *hw = &i915->hw;
    uint32_t buf = i915_read(hw, DDI_BUF_CTL(port));
    int wait = 0;

    if (buf & DDI_BUF_CTL_ENABLE) {
        i915_write(hw, DDI_BUF_CTL(port), buf & ~DDI_BUF_CTL_ENABLE);
        wait = 1;
    }
    if (dp) {
        i915_rmw(hw, DP_TP_CTL(port), DP_TP_CTL_ENABLE, 0);
    }
    if (wait) {
        wait_buf_idle(i915, port);
    }
}

void intel_ddi_post_disable(struct drm_i915_private *i915, struct intel_crtc *crtc,
                            const struct intel_crtc_config *cfg)
{
    struct i915_hw *hw = &i915->hw;
    struct intel_dp *dp;
    uint32_t conf, ctl;

    intel_crtc_vblank_off(crtc);
    /* Linux's intel_disable_transcoder. */
    conf = i915_read(hw, TRANSCONF(cfg->trans));
    if (conf & TRANSCONF_ENABLE) {
        i915_write(hw, TRANSCONF(cfg->trans), conf & ~TRANSCONF_ENABLE);
        if (i915_wait_for(hw, "pipe off", TRANSCONF(cfg->trans), TRANSCONF_STATE_ENABLE, 0,
                          PIPE_OFF_US) != 0) {
            klog(KLOG_ERR, "i915", "ddi: pipe %c off wait timed out", 'A' + crtc->pipe);
        }
    }
    /* Linux's intel_ddi_disable_transcoder_func. */
    ctl = i915_read(hw, TRANS_DDI_FUNC_CTL(cfg->trans));
    ctl &= ~(TRANS_DDI_FUNC_ENABLE | TRANS_DDI_PORT_MASK | TRANS_DDI_MODE_SELECT_MASK);
    i915_write(hw, TRANS_DDI_FUNC_CTL(cfg->trans), ctl);
    ilk_pfit_disable(i915, crtc->pipe, cfg);
    if (cfg->output == INTEL_OUTPUT_HDMI) {
        /* Linux's intel_ddi_post_disable_hdmi. */
        intel_hdmi_set_infoframes(i915, 0, cfg);
        disable_transcoder_clock(i915, cfg);
        disable_buf(i915, cfg->port, 0);
        i915_write(hw, PORT_CLK_SEL(cfg->port), PORT_CLK_SEL_NONE);
        intel_hdmi_dual_mode_tmds(&i915->display.ddi[cfg->port - 1], 0);
        return;
    }
    /* Linux's intel_ddi_post_disable_dp: the sink powered down before the
     * port, lest it signal the link's loss, and before Gen 12 the
     * transcoder's clock taken before the buffer goes off. */
    dp = port_dp(i915, cfg->port);
    clear_infoframes(i915, cfg->trans);
    intel_dp_set_power(dp, DP_SET_POWER_D3);
    disable_transcoder_clock(i915, cfg);
    disable_buf(i915, dp->port, 1);
    dp->buf_ctl &= ~DDI_BUF_CTL_ENABLE;
    if (dp->edp) {
        (void)intel_pps_vdd_on_unlocked(i915);
        intel_pps_off(i915);
    }
    i915_write(hw, PORT_CLK_SEL(dp->port), PORT_CLK_SEL_NONE);
}
