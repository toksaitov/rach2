/*
 * intel_dp.c: DisplayPort on Haswell's DDI ports, after Linux's
 * display/intel_dp.c: the eDP panel on DDI A, read once at init, and the
 * monitors on DDI B to D, probed on each hotplug; what a sink can do, its
 * connector, the link and pipe configuration a mode takes, the sink's
 * power state, and its hotplug pulses.
 */

#include "i915.h"
#include "i915_reg.h"
#include "intel_dp.h"

#include "../../klog.h"
#include "../../lib/kerrno.h"
#include "../../lib/kstring.h"

/* The link rates in kHz of symbol clock Haswell's DDI drives, and those
 * DP_MAX_LINK_RATE may name, below the 128b/132b rates. */
static const uint32_t hsw_rates[] = {162000u, 270000u, 540000u};
static const uint32_t dp_rates[] = {162000u, 270000u, 540000u, 810000u};

/* Intel's IEEE OUI, which Linux writes as the source's. */
static const uint8_t intel_oui[3] = {0x00, 0xaa, 0x01};

/** A register of the device. */
static uint32_t dp_rd(struct drm_i915_private *i915, uint32_t reg)
{
    return i915_read(&i915->hw, reg);
}

/* ------------------------------------------------------------------------
 * The sink's capabilities
 */

/** Read the receiver's capabilities into `dpcd`, Linux's
 * drm_dp_read_dpcd_caps with the extended ones of DPCD 1.3 when the sink
 * has them. 0, or -EIO for a sink that does not answer or has no DPCD. */
static int32_t read_dpcd_caps(struct intel_dp *dp)
{
    uint8_t ext[DP_RECEIVER_CAP_SIZE];

    if (intel_dp_dpcd_read(dp, DP_DPCD_REV, dp->dpcd, DP_RECEIVER_CAP_SIZE) !=
            DP_RECEIVER_CAP_SIZE ||
        dp->dpcd[DP_DPCD_REV] == 0) {
        return -EIO;
    }
    if (!(dp->dpcd[DP_TRAINING_AUX_RD_INTERVAL] & DP_EXTENDED_RECEIVER_CAP_FIELD_PRESENT)) {
        return 0;
    }
    if (intel_dp_dpcd_read(dp, DP_DP13_DPCD_REV, ext, sizeof(ext)) != (int32_t)sizeof(ext)) {
        return -EIO;
    }
    /* Linux keeps the base capabilities when the extended ones claim an
     * older revision. */
    if (dp->dpcd[DP_DPCD_REV] <= ext[DP_DPCD_REV]) {
        k_memcpy(dp->dpcd, ext, sizeof(ext));
    }
    return 0;
}

/** Write Intel's OUI as the source's, Linux's intel_edp_init_source_oui;
 * `careful` leaves one already written alone, as at init. */
static void init_source_oui(struct intel_dp *dp, int careful)
{
    uint8_t buf[3] = {0, 0, 0};

    if (careful) {
        if (intel_dp_dpcd_read(dp, DP_SOURCE_OUI, buf, sizeof(buf)) < 0) {
            klog(KLOG_ERR, "i915", "%s: failed to read source OUI", dp->name);
        }
        if (buf[0] == intel_oui[0] && buf[1] == intel_oui[1] && buf[2] == intel_oui[2]) {
            return;
        }
    }
    if (intel_dp_dpcd_write(dp, DP_SOURCE_OUI, intel_oui, sizeof(intel_oui)) < 0) {
        klog(KLOG_ERR, "i915", "%s: failed to write source OUI", dp->name);
    }
}

/** The sink's rates from DP_MAX_LINK_RATE, Linux's intel_dp_set_sink_rates;
 * 1.62 Gb/s alone for a sink that names none. */
static void set_sink_rates(struct intel_dp *dp)
{
    uint32_t max = (uint32_t)dp->dpcd[DP_MAX_LINK_RATE] * 27000u;
    int i;

    for (i = 0; i < (int)(sizeof(dp_rates) / sizeof(dp_rates[0])) && dp_rates[i] <= max; i++) {
        dp->sink_rates[i] = dp_rates[i];
    }
    dp->nsink_rates = i;
    if (i == 0) {
        klog(KLOG_ERR, "i915", "%s: invalid DPCD with no link rates, using defaults", dp->name);
        dp->sink_rates[0] = 162000u;
        dp->nsink_rates = 1;
    }
}

/**
 * Read the panel's DPCD, Linux's intel_edp_init_dpcd: the receiver's
 * capabilities, eDP's display control capabilities, and the link rates,
 * from eDP 1.4's table when the sink has one, set then by index. A panel
 * before eDP 1.4 trains first at its most (`use_max_params`). 0 or -EIO.
 */
static int32_t init_dpcd(struct intel_dp *dp)
{
    uint8_t table[2 * DP_MAX_SUPPORTED_RATES];
    uint32_t val;
    int i;

    if (read_dpcd_caps(dp) != 0) {
        return -EIO;
    }
    /* Read whatever DP_EDP_CONFIGURATION_CAP says, since some panels
     * leave its bit clear and still need eDP 1.4's detection; the
     * registers read 0 where they are missing. */
    if (intel_dp_dpcd_read(dp, DP_EDP_DPCD_REV, dp->edp_dpcd, EDP_DISPLAY_CTL_CAP_SIZE) ==
        EDP_DISPLAY_CTL_CAP_SIZE) {
        dp->use_max_params = dp->edp_dpcd[0] < DP_EDP_14;
    }
    dp->nsink_rates = 0;
    dp->use_rate_select = 0;
    if (dp->edp_dpcd[0] >= DP_EDP_14) {
        k_memset(table, 0, sizeof(table));
        (void)intel_dp_dpcd_read(dp, DP_SUPPORTED_LINK_RATES, table, sizeof(table));
        for (i = 0; i < DP_MAX_SUPPORTED_RATES; i++) {
            val = (uint32_t)table[2 * i] | (uint32_t)table[2 * i + 1] << 8;
            if (val == 0) {
                break;
            }
            /* 200 kHz units of the bit rate, to kHz of symbol clock. */
            dp->sink_rates[i] = val * 200u / 10u;
        }
        dp->nsink_rates = i;
    }
    if (dp->nsink_rates != 0) {
        dp->use_rate_select = 1;
    } else {
        set_sink_rates(dp);
    }
    dp->max_lanes = dp->dpcd[DP_MAX_LANE_COUNT] & DP_MAX_LANE_COUNT_MASK;
    init_source_oui(dp, 1);
    return 0;
}

/** The fastest rate in kHz the VBT lets the port drive, Linux's
 * vbt_max_link_rate, with the eDP block's bound for the panel; 0 for no
 * limit. */
static uint32_t vbt_max_link_rate(const struct intel_dp *dp, const struct intel_bios_encoder *enc)
{
    uint32_t max = enc ? enc->dp_max_link_rate : 0, edp = dp->edp ? dp->vbt.edp.max_link_rate : 0;

    if (max && edp) {
        return max < edp ? max : edp;
    }
    return max ? max : edp;
}

/** The rates both the port and the sink take, lowest first, Linux's
 * intel_dp_set_source_rates and intel_dp_set_common_rates. */
static void set_common_rates(struct intel_dp *dp)
{
    const struct intel_bios_encoder *enc = intel_bios_encoder_lookup(&dp->i915->vbt, dp->port);
    uint32_t max = vbt_max_link_rate(dp, enc);
    int i, j, nsource = 0;

    for (i = 0; i < (int)(sizeof(hsw_rates) / sizeof(hsw_rates[0])); i++) {
        if (max == 0 || hsw_rates[i] <= max) {
            nsource = i + 1;
        }
    }
    dp->nrates = 0;
    for (i = 0; i < nsource; i++) {
        for (j = 0; j < dp->nsink_rates; j++) {
            if (hsw_rates[i] == dp->sink_rates[j] && dp->nrates < 8) {
                dp->rates[dp->nrates++] = hsw_rates[i];
            }
        }
    }
    if (dp->nrates == 0) {
        dp->rates[0] = 162000u;
        dp->nrates = 1;
    }
}

/** The most lanes both the port and the sink have, Linux's
 * intel_dp_max_common_lane_count with intel_ddi_max_lanes: DDI A has 4
 * only when the firmware gave it DDI E's, B to D have 4, and the VBT may
 * lower it. */
static uint32_t max_common_lanes(const struct intel_dp *dp)
{
    const struct intel_bios_encoder *enc = intel_bios_encoder_lookup(&dp->i915->vbt, dp->port);
    uint32_t source = 4u;

    if (dp->port == INTEL_PORT_A && !(dp->saved_port_bits & DDI_A_4_LANES)) {
        source = 2u;
    }
    if (enc && enc->dp_max_lane_count && enc->dp_max_lane_count < source) {
        source = enc->dp_max_lane_count;
    }
    return dp->max_lanes < source ? dp->max_lanes : source;
}

/** The panel's colour depth in bits from its EDID, Linux's
 * drm_parse_display_info for EDID 1.4; 0 when it says none. */
static uint32_t edid_bpc(const struct intel_dp *dp)
{
    static const uint8_t bpc[8] = {0, 6, 8, 10, 12, 14, 16, 0};
    const uint8_t *e = dp->edid;

    if (dp->edid_len < 128 || e[0x12] != 1 || e[0x13] < 4 || !(e[0x14] & 0x80)) {
        return 0;
    }
    return bpc[(e[0x14] >> 4) & 7u];
}

int32_t intel_dp_init(struct drm_i915_private *i915)
{
    struct intel_dp *dp = &i915->display.dp;
    const struct intel_bios_encoder *enc = intel_bios_encoder_lookup(&i915->vbt, INTEL_PORT_A);
    uint32_t buf = dp_rd(i915, DDI_BUF_CTL(INTEL_PORT_A));

    dp->i915 = i915;
    dp->name = "edp";
    dp->edp = 1;
    dp->connector = &i915->display.connector;

    /* Linux's intel_setup_outputs on Haswell: DDI A is set up when the
     * strap says a display is there, and it is always eDP. */
    if (!(buf & DDI_INIT_DISPLAY_DETECTED)) {
        klog(KLOG_INFO, "i915", "edp: no display detected on DDI A");
        return -ENODEV;
    }
    dp->port = INTEL_PORT_A;
    dp->aux_port = enc && enc->aux_ch != INTEL_AUX_CH_NONE ? enc->aux_ch : INTEL_PORT_A;
    dp->saved_port_bits = buf & (DDI_BUF_PORT_REVERSAL | DDI_A_4_LANES);
    if (enc && enc->lane_reversal) {
        dp->saved_port_bits |= DDI_BUF_PORT_REVERSAL;
    }
    dp->vbt.panel_type = -1;
    intel_bios_panel_init(&i915->vbt, &dp->vbt, enc, NULL, 0);
    intel_pps_init(i915);
    if (init_dpcd(dp) != 0) {
        klog(KLOG_INFO, "i915", "edp: failed to retrieve link info, disabling eDP");
        intel_pps_vdd_off_unlocked(i915, 1);
        return -ENODEV;
    }
    if (intel_dp_read_edid(dp, dp->edid, sizeof(dp->edid), &dp->edid_len) != 0) {
        dp->edid_len = 0;
    }
    intel_bios_panel_init(&i915->vbt, &dp->vbt, enc, dp->edid_len ? dp->edid : NULL, 1);
    intel_panel_init_fixed_modes(i915);
    if (dp->nfixed == 0) {
        klog(KLOG_INFO, "i915", "edp: failed to find fixed mode for the panel, disabling eDP");
        intel_pps_vdd_off_unlocked(i915, 1);
        return -ENODEV;
    }
    intel_pps_init_late(i915);
    set_common_rates(dp);
    dp->edid_bpc = edid_bpc(dp);
    dp->present = 1;
    klog(KLOG_INFO, "i915",
         "edp: DPCD %x.%x, eDP %u, %u lane%s, rates %u to %u kHz%s, %u bpc, EDID %u bytes",
         (unsigned)(dp->dpcd[DP_DPCD_REV] >> 4), (unsigned)(dp->dpcd[DP_DPCD_REV] & 0xfu),
         (unsigned)dp->edp_dpcd[0], (unsigned)max_common_lanes(dp),
         max_common_lanes(dp) == 1 ? "" : "s", (unsigned)dp->rates[0],
         (unsigned)dp->rates[dp->nrates - 1], dp->use_rate_select ? " by index" : "",
         (unsigned)dp->edid_bpc, (unsigned)dp->edid_len);
    return 0;
}

/* ------------------------------------------------------------------------
 * The sink's power
 */

/** Whether the sink is a branch device, Linux's drm_dp_is_branch. */
static int is_branch(const uint8_t *dpcd)
{
    return (dpcd[DP_DOWNSTREAMPORT_PRESENT] & DP_DWN_STRM_PORT_PRESENT) != 0;
}

/** Whether a branch's first port needs the sink in D0 to see its own
 * hotplug, Linux's downstream_hpd_needs_d0: DPCD 1.1 with HPD there. */
static int downstream_hpd_needs_d0(const struct intel_dp *dp)
{
    return dp->dpcd[DP_DPCD_REV] == DP_DPCD_REV_11 && is_branch(dp->dpcd) &&
           (dp->downstream_ports[0] & DP_DS_PORT_HPD);
}

void intel_dp_set_power(struct intel_dp *dp, uint8_t mode)
{
    int32_t ret = 0;
    int i;

    if (dp->dpcd[DP_DPCD_REV] < DP_DPCD_REV_11) {
        return;
    }
    if (mode != DP_SET_POWER_D0) {
        if (downstream_hpd_needs_d0(dp)) {
            return;
        }
        ret = intel_dp_dpcd_writeb(dp, DP_SET_POWER, mode);
    } else {
        /* Linux writes the source's OUI for eDP alone. */
        if (dp->edp) {
            init_source_oui(dp, 0);
        }
        /* The sink may take a millisecond to wake. */
        for (i = 0; i < 3; i++) {
            ret = intel_dp_dpcd_writeb(dp, DP_SET_POWER, mode);
            if (ret == 1) {
                break;
            }
            intel_display_msleep(dp->i915, 1);
        }
    }
    if (ret != 1) {
        klog(KLOG_WARN, "i915", "%s: failed to %s sink power state", dp->name,
             mode == DP_SET_POWER_D0 ? "enable" : "disable");
    }
}

/* ------------------------------------------------------------------------
 * A mode's configuration
 */

/** Linux's intel_dp_link_required: the thousands of bytes a second a
 * mode of `clock` kHz takes at `bpp` bits a pixel. */
static uint32_t link_required(uint32_t clock, uint32_t bpp)
{
    return (clock * bpp + 7u) / 8u;
}

/** Linux's drm_dp_max_dprx_data_rate for 8b/10b: the thousands of bytes a
 * second `lanes` lanes carry at `rate` kHz of symbol clock, a byte a
 * symbol. */
static uint32_t max_data_rate(uint32_t rate, uint32_t lanes)
{
    return rate * lanes;
}

/** The pipe's bits a pixel the connector allows, Linux's
 * compute_baseline_pipe_bpp and compute_sink_pipe_bpp: 36 on Haswell, cut
 * to the sink's colour depth, 8 bits when its EDID says none, and to "max
 * bpc". */
static uint32_t sink_pipe_bpp(const struct intel_dp *dp, const struct drm_connector_state *conn)
{
    uint32_t bpc = dp->edid_bpc ? dp->edid_bpc : 8u, bpp = 36u;

    if (conn->max_requested_bpc && conn->max_requested_bpc < bpc) {
        bpc = conn->max_requested_bpc;
    }
    if (bpc < 8) {
        bpc = 6;
    } else if (bpc < 10) {
        bpc = 8;
    } else if (bpc < 12) {
        bpc = 10;
    } else {
        bpc = 12;
    }
    return 3u * bpc < bpp ? 3u * bpc : bpp;
}

/** Linux's intel_hdmi_tmds_clock for RGB: the TMDS clock in kHz of a mode
 * of `clock` kHz at `bpc` bits a colour. */
static uint32_t tmds_clock(uint32_t clock, uint32_t bpc)
{
    return (clock * bpc + 4u) / 8u;
}

/** Whether a branch's TMDS port carries `clock` kHz at `bpc`, Linux's
 * intel_dp_tmds_clock_valid; always when `respect` is 0. */
static int tmds_clock_valid(const struct intel_dp *dp, uint32_t clock, uint32_t bpc, int respect)
{
    uint32_t tmds = tmds_clock(clock, bpc);

    if (!respect) {
        return 1;
    }
    if (dp->dfp_min_tmds && tmds < dp->dfp_min_tmds) {
        return 0;
    }
    return !dp->dfp_max_tmds || tmds <= dp->dfp_max_tmds;
}

/** The most bits a colour, from `bpc` down, an HDMI or DVI sink behind a
 * branch takes at `clock` kHz, Linux's intel_dp_hdmi_compute_bpc: 8 at
 * least, 8 alone when the branch's limits are not kept, and 10 or 12 only
 * for an HDMI sink with deep colour. -1 for none. */
static int hdmi_compute_bpc(const struct intel_dp *dp, uint32_t clock, int bpc, int respect)
{
    if (bpc < 8 || !respect) {
        bpc = 8;
    }
    for (; bpc >= 8; bpc -= 2) {
        int possible = bpc == 8 || (bpc == 10 && dp->info.is_hdmi && dp->info.dc_30) ||
                       (bpc == 12 && dp->info.is_hdmi && dp->info.dc_36);

        if (possible && tmds_clock_valid(dp, clock, (uint32_t)bpc, respect)) {
            return bpc;
        }
    }
    return -1;
}

/** The most bits a pixel the link may carry for `cfg`'s mode, Linux's
 * intel_dp_max_bpp: the sink's, a branch's port's depth and TMDS clocks,
 * and for a panel whose EDID names no depth the VBT's; 0 for none. */
static uint32_t max_bpp(const struct intel_dp *dp, const struct drm_connector_state *conn,
                        const struct intel_crtc_config *cfg, int respect)
{
    int bpc = (int)sink_pipe_bpp(dp, conn) / 3, hdmi;
    uint32_t bpp;

    if (dp->dfp_max_bpc && (int)dp->dfp_max_bpc < bpc) {
        bpc = (int)dp->dfp_max_bpc;
    }
    if (dp->dfp_min_tmds) {
        hdmi = hdmi_compute_bpc(dp, cfg->mode.clock, bpc, respect);
        if (hdmi < 0) {
            return 0;
        }
        bpc = hdmi < bpc ? hdmi : bpc;
    }
    bpp = 3u * (uint32_t)bpc;
    if (dp->edp && dp->edid_bpc == 0 && dp->vbt.edp.bpp && dp->vbt.edp.bpp < bpp) {
        bpp = dp->vbt.edp.bpp;
    }
    return bpp;
}

/** Linux's compute_m_n: `m`/`n` with `n` fixed at `constant_n`, both
 * halved until they fit their 24 bits. */
static void compute_m_n(uint32_t *ret_m, uint32_t *ret_n, uint32_t m, uint32_t n,
                        uint32_t constant_n)
{
    *ret_n = constant_n;
    *ret_m = (uint32_t)drm_div64((uint64_t)m * constant_n, n);
    while (*ret_m > DATA_LINK_M_N_MASK || *ret_n > DATA_LINK_M_N_MASK) {
        *ret_m >>= 1;
        *ret_n >>= 1;
    }
}

/**
 * Choose the link for `cfg`'s mode, Linux's intel_dp_compute_link_config
 * with intel_dp_compute_link_config_wide: the most bits a pixel first, from
 * the most `max_bpp` allows down to 18 in steps of 6, then the slowest
 * rate, then the fewest lanes that carry it. A panel that failed to train
 * at the least takes its most (`use_max_params`), and a fallback bounds
 * the link; `respect` keeps a branch's limits. 0, or -EINVAL when no link
 * carries the mode.
 */
static int32_t compute_link(struct intel_dp *dp, const struct drm_connector_state *conn,
                            struct intel_crtc_config *cfg, int respect)
{
    uint32_t clock = cfg->mode.clock, max_rate, min_rate, max_lanes, min_lanes, lanes, bpp;
    int i;

    max_rate = dp->link_max_rate ? dp->link_max_rate : dp->rates[dp->nrates - 1];
    max_lanes = dp->link_max_lanes ? dp->link_max_lanes : max_common_lanes(dp);
    min_rate = dp->rates[0] < max_rate ? dp->rates[0] : max_rate;
    min_lanes = 1;
    if (dp->use_max_params) {
        min_rate = max_rate;
        min_lanes = max_lanes;
    }
    bpp = max_bpp(dp, conn, cfg, respect);
    bpp -= bpp % 6u;
    for (; bpp >= 18u; bpp -= 6u) {
        for (i = 0; i < dp->nrates; i++) {
            if (dp->rates[i] < min_rate || dp->rates[i] > max_rate) {
                continue;
            }
            for (lanes = min_lanes; lanes <= max_lanes; lanes <<= 1) {
                if (link_required(clock, bpp) <= max_data_rate(dp->rates[i], lanes)) {
                    cfg->bpp = bpp;
                    cfg->port_clock = dp->rates[i];
                    cfg->lanes = lanes;
                    return 0;
                }
            }
        }
    }
    return -EINVAL;
}

int32_t intel_dp_compute_config(struct intel_dp *dp, int pipe,
                                const struct drm_connector_state *conn, int crc_wa,
                                struct intel_crtc_config *cfg)
{
    const struct drm_mode_modeinfo *m = &cfg->mode;
    uint32_t data_m;
    int32_t err;

    if (dp->edp && (err = intel_panel_compute_config(dp->i915, &cfg->mode)) != 0) {
        return err;
    }
    /* DBLSCAN and DBLCLK nobody; interlace a monitor's connector alone. */
    if ((m->flags & (DRM_MODE_FLAG_DBLSCAN | DRM_MODE_FLAG_DBLCLK)) ||
        ((m->flags & DRM_MODE_FLAG_INTERLACE) && !dp->connector->interlace_allowed)) {
        return -EINVAL;
    }
    /* A branch's TMDS limits first; then on the chance the user knows
     * better. */
    err = compute_link(dp, conn, cfg, 1);
    if (err != 0) {
        err = compute_link(dp, conn, cfg, 0);
    }
    if (err != 0) {
        return err;
    }
    if (dp->edp) {
        err = intel_pfit_compute_config(pipe, conn->scaling_mode, cfg);
        if (err != 0) {
            return err;
        }
    }
    cfg->output = dp->edp ? INTEL_OUTPUT_EDP : INTEL_OUTPUT_DP;
    cfg->port = dp->port;
    cfg->trans = dp->edp ? I915_TRANS_EDP : pipe;
    cfg->dither = cfg->bpp == 18u;
    /* Linux's intel_dp_limited_color_range with "Broadcast RGB" on
     * automatic: CEA-861's limited range for a CEA mode but the first,
     * above 6 bits a colour. */
    cfg->limited_range = cfg->bpp != 18u && drm_default_rgb_quant_limited(&cfg->mode);
    /* Linux's intel_link_compute_m_n with the fixed N of Windows and the
     * firmware, and a transfer unit of 64. */
    data_m = link_required(m->clock, cfg->bpp);
    cfg->tu = 64;
    compute_m_n(&cfg->data_m, &cfg->data_n, data_m, cfg->port_clock * cfg->lanes, 0x8000000u);
    compute_m_n(&cfg->link_m, &cfg->link_n, m->clock, cfg->port_clock, 0x80000u);
    /* Pipe A drives the eDP transcoder through the fitter's power well
     * while the fitter or a CRC of it runs (Linux's force_thru). */
    cfg->force_thru = dp->edp && pipe == I915_PIPE_A && (cfg->pfit || crc_wa);
    return 0;
}

/* ------------------------------------------------------------------------
 * The connector
 */

/** The panel is there: Linux's edp_detect, with the VDD the probe let go
 * turned off once its time came. */
static int intel_edp_detect(struct drm_connector *connector, int force_probe)
{
    struct drm_i915_private *i915 = to_i915(connector->dev);

    (void)force_probe;
    intel_pps_vdd_work(i915);
    return DRM_CONNECTOR_CONNECTED;
}

/** The modes of the panel's EDID, read at init, and its fixed modes,
 * Linux's intel_dp_get_modes. */
static int intel_edp_get_modes(struct drm_connector *connector)
{
    struct drm_i915_private *i915 = to_i915(connector->dev);
    struct intel_dp *dp = &i915->display.dp;
    int n;

    n = drm_connector_update_edid(connector, dp->edid_len ? dp->edid : NULL, dp->edid_len);
    return n + intel_panel_get_modes(i915, connector);
}

/** The link's most rate and lanes now, Linux's intel_dp_max_link_rate and
 * intel_dp_max_lane_count. */
static void link_max(const struct intel_dp *dp, uint32_t *rate, uint32_t *lanes)
{
    *rate = dp->link_max_rate ? dp->link_max_rate : dp->rates[dp->nrates - 1];
    *lanes = dp->link_max_lanes ? dp->link_max_lanes : max_common_lanes(dp);
}

/** A mode the panel shows, Linux's intel_dp_mode_valid for eDP: the
 * transcoder's limits, a panel's size and rate, its fixed mode's clock
 * within the CD clock, and a link that carries it at 18 bits a pixel. */
static int32_t intel_edp_mode_valid(struct drm_connector *connector,
                                    const struct drm_mode_modeinfo *mode)
{
    struct drm_i915_private *i915 = to_i915(connector->dev);
    struct intel_dp *dp = &i915->display.dp;
    const struct drm_mode_modeinfo *fixed;
    uint32_t clock = mode->clock, rate, lanes;

    if (intel_cpu_transcoder_mode_valid(mode) != 0 || (mode->flags & DRM_MODE_FLAG_DBLCLK) ||
        mode->clock < 10000u) {
        return -EINVAL;
    }
    fixed = intel_panel_fixed_mode(i915, mode);
    if (fixed) {
        if (intel_panel_mode_valid(i915, mode) != 0) {
            return -EINVAL;
        }
        clock = fixed->clock;
    }
    if (clock > intel_max_dotclock(i915)) {
        return -EINVAL;
    }
    link_max(dp, &rate, &lanes);
    return link_required(clock, 18u) <= max_data_rate(rate, lanes) ? 0 : -EINVAL;
}

static const struct drm_connector_funcs intel_edp_funcs = {intel_edp_detect, intel_edp_get_modes,
                                                           intel_edp_mode_valid, NULL};

int32_t intel_dp_connector_init(struct drm_i915_private *i915)
{
    struct intel_display *d = &i915->display;
    struct drm_device *dev = &i915->drm;
    int32_t err;

    if ((err = drm_encoder_init(dev, &d->encoder, DRM_MODE_ENCODER_TMDS)) != 0 ||
        (err = drm_connector_init(dev, &d->connector, &intel_edp_funcs, DRM_MODE_CONNECTOR_eDP)) !=
            0) {
        return err;
    }
    /* Haswell's eDP transcoder takes any pipe. */
    d->encoder.possible_crtcs = 0x7u;
    d->connector.status = DRM_CONNECTOR_CONNECTED;
    d->connector.interlace_allowed = 0;
    d->connector.doublescan_allowed = 0;
    if ((err = drm_connector_attach_encoder(&d->connector, &d->encoder)) != 0) {
        return err;
    }
    /* Linux's intel_dp_add_properties for eDP on a PCH display: "max bpc"
     * from 6 to 12, and intel_attach_scaling_mode_property's "scaling mode",
     * Full aspect first, Full and Center, without None. */
    if ((err = drm_connector_attach_max_bpc_property(&d->connector, 6, 12)) != 0 ||
        (err = drm_connector_attach_scaling_mode_property(
             &d->connector, 1u << DRM_MODE_SCALE_ASPECT | 1u << DRM_MODE_SCALE_FULLSCREEN |
                                1u << DRM_MODE_SCALE_CENTER)) != 0) {
        return err;
    }
    d->connector.state->scaling_mode = DRM_MODE_SCALE_ASPECT;
    return 0;
}

/* ------------------------------------------------------------------------
 * The monitors on DDI B to D
 */

/** The DisplayPort sink of the monitor connector `connector`. */
static struct intel_dp *ext_dp(struct drm_connector *connector)
{
    struct drm_i915_private *i915 = to_i915(connector->dev);
    int i;

    for (i = 0; i < 3; i++) {
        if (connector == &i915->display.ddi[i].dp_connector) {
            return &i915->display.ddi[i].dp;
        }
    }
    return &i915->display.dp;
}

int intel_dp_connected(struct intel_dp *dp)
{
    return dp->edp || intel_hotplug_live(dp->i915, dp->port - INTEL_PORT_B);
}

/** Whether the sink's SINK_COUNT is to be trusted, Linux's
 * drm_dp_read_sink_count_cap: a branch of DPCD 1.1 or later, not a panel. */
static int has_sink_count(const struct intel_dp *dp)
{
    return !dp->edp && dp->dpcd[DP_DPCD_REV] >= DP_DPCD_REV_11 && is_branch(dp->dpcd);
}

/** Read a branch's downstream ports' capabilities, Linux's
 * drm_dp_read_downstream_info: one byte each, four with the detailed
 * capabilities. 0 or -EIO. */
static int32_t read_downstream_info(struct intel_dp *dp)
{
    uint32_t len;

    k_memset(dp->downstream_ports, 0, sizeof(dp->downstream_ports));
    if (!is_branch(dp->dpcd) || dp->dpcd[DP_DPCD_REV] == 0x10) {
        return 0;
    }
    /* Some branches say they have no port beside a port present. */
    len = dp->dpcd[DP_DOWN_STREAM_PORT_COUNT] & DP_PORT_COUNT_MASK;
    if (len == 0) {
        return 0;
    }
    if (dp->dpcd[DP_DOWNSTREAMPORT_PRESENT] & DP_DETAILED_CAP_INFO_AVAILABLE) {
        len *= 4u;
    }
    return intel_dp_dpcd_read(dp, DP_DOWNSTREAM_PORT_0, dp->downstream_ports, len) == (int32_t)len
               ? 0
               : -EIO;
}

/**
 * Read a monitor's DPCD, Linux's intel_dp_get_dpcd for an external port:
 * the receiver's capabilities, the rates and lanes both ends take, the
 * sinks behind a branch, 0 meaning a branch with nothing behind it, and
 * the branch's ports. 1 when a sink is there, 0 otherwise.
 */
static int get_dpcd(struct intel_dp *dp)
{
    uint8_t count;

    if (read_dpcd_caps(dp) != 0) {
        return 0;
    }
    set_sink_rates(dp);
    dp->max_lanes = dp->dpcd[DP_MAX_LANE_COUNT] & DP_MAX_LANE_COUNT_MASK;
    set_common_rates(dp);
    if (has_sink_count(dp)) {
        if (intel_dp_dpcd_read(dp, DP_SINK_COUNT, &count, 1) != 1) {
            return 0;
        }
        dp->sink_count = (uint8_t)DP_GET_SINK_COUNT(count);
        if (dp->sink_count == 0) {
            return 0;
        }
    }
    return read_downstream_info(dp) == 0;
}

/**
 * Whether a monitor is there by its DPCD, Linux's intel_dp_detect_dpcd: a
 * sink that is no branch is; a branch whose ports have hotplug says by its
 * sink count; one that answers on its DDC is; a VGA or non-EDID port is
 * unknown, and anything else out of the standard and not there.
 */
static int detect_dpcd(struct intel_dp *dp)
{
    uint8_t type;

    if (!get_dpcd(dp)) {
        return DRM_CONNECTOR_DISCONNECTED;
    }
    if (!is_branch(dp->dpcd)) {
        return DRM_CONNECTOR_CONNECTED;
    }
    if (has_sink_count(dp) && (dp->downstream_ports[0] & DP_DS_PORT_HPD)) {
        return dp->sink_count ? DRM_CONNECTOR_CONNECTED : DRM_CONNECTOR_DISCONNECTED;
    }
    if (intel_dp_probe_ddc(dp)) {
        return DRM_CONNECTOR_CONNECTED;
    }
    if (dp->dpcd[DP_DPCD_REV] >= DP_DPCD_REV_11) {
        type = dp->downstream_ports[0] & DP_DS_PORT_TYPE_MASK;
        if (type == DP_DS_PORT_TYPE_VGA || type == DP_DS_PORT_TYPE_NON_EDID) {
            return DRM_CONNECTOR_UNKNOWN;
        }
    } else {
        type = dp->dpcd[DP_DOWNSTREAMPORT_PRESENT] & DP_DWN_STRM_PORT_TYPE_MASK;
        if (type == DP_DWN_STRM_PORT_TYPE_ANALOG || type == DP_DWN_STRM_PORT_TYPE_OTHER) {
            return DRM_CONNECTOR_UNKNOWN;
        }
    }
    klog(KLOG_INFO, "i915", "%s: broken DP branch device, ignoring", dp->name);
    return DRM_CONNECTOR_DISCONNECTED;
}

/** Whether the EDID in use, the override or the sink's, is EDID 1.4's
 * digital DisplayPort input, Linux's is_edid_digital_input_dp. */
static int edid_digital_dp(const struct intel_dp *dp)
{
    const uint8_t *e = dp->connector->edid_override ? dp->connector->edid_override : dp->edid;
    uint32_t len = dp->connector->edid_override ? dp->connector->edid_override_len : dp->edid_len;

    return len >= 128 && e[0x13] >= 4 && (e[0x14] & 0x80) && (e[0x14] & 7u) == 5u;
}

/** A branch's first port's capabilities, Linux's intel_dp_update_dfp with
 * drm_dp_downstream_max_bpc, max_dotclock, min_tmds_clock and
 * max_tmds_clock; 0 each for a sink that is no branch. */
static void update_dfp(struct intel_dp *dp)
{
    const uint8_t *cap = dp->downstream_ports;
    uint32_t detailed = dp->dpcd[DP_DOWNSTREAMPORT_PRESENT] & DP_DETAILED_CAP_INFO_AVAILABLE;
    uint32_t type;

    dp->dfp_max_bpc = dp->dfp_max_dotclock = dp->dfp_min_tmds = dp->dfp_max_tmds = 0;
    if (!is_branch(dp->dpcd)) {
        return;
    }
    if (dp->dpcd[DP_DPCD_REV] < DP_DPCD_REV_11) {
        type = dp->dpcd[DP_DOWNSTREAMPORT_PRESENT] & DP_DWN_STRM_PORT_TYPE_MASK;
        dp->dfp_max_bpc = type == DP_DWN_STRM_PORT_TYPE_DP ? 0 : 8;
        if (type == DP_DWN_STRM_PORT_TYPE_TMDS) {
            dp->dfp_min_tmds = 25000;
            dp->dfp_max_tmds = 165000;
        }
        return;
    }
    type = cap[0] & DP_DS_PORT_TYPE_MASK;
    if (type == DP_DS_PORT_TYPE_VGA && detailed) {
        dp->dfp_max_dotclock = cap[1] * 8000u;
    }
    if (type == DP_DS_PORT_TYPE_DP_DUALMODE && edid_digital_dp(dp)) {
        return;
    }
    switch (type) {
    case DP_DS_PORT_TYPE_DP:
        return;
    case DP_DS_PORT_TYPE_DP_DUALMODE:
    case DP_DS_PORT_TYPE_HDMI:
        /* 300 MHz without the detailed capabilities, as Linux assumes an
         * HDMI 1.4 converter. */
        dp->dfp_min_tmds = 25000;
        dp->dfp_max_tmds = detailed ? cap[1] * 2500u : 300000u;
        break;
    case DP_DS_PORT_TYPE_DVI:
        dp->dfp_min_tmds = 25000;
        dp->dfp_max_tmds = detailed ? cap[1] * 2500u : 165000u;
        break;
    default:
        break;
    }
    dp->dfp_max_bpc = 8;
    if (detailed && (type == DP_DS_PORT_TYPE_DP_DUALMODE || type == DP_DS_PORT_TYPE_HDMI ||
                     type == DP_DS_PORT_TYPE_DVI || type == DP_DS_PORT_TYPE_VGA)) {
        static const uint8_t bpc[4] = {8, 10, 12, 16};

        dp->dfp_max_bpc = bpc[cap[2] & DP_DS_MAX_BPC_MASK];
    }
}

/** Forget a monitor's EDID and what it said, Linux's intel_dp_unset_edid. */
static void unset_edid(struct intel_dp *dp)
{
    dp->edid_len = 0;
    dp->edid_bpc = 0;
    k_memset(&dp->info, 0, sizeof(dp->info));
    dp->dfp_max_bpc = dp->dfp_max_dotclock = dp->dfp_min_tmds = dp->dfp_max_tmds = 0;
}

/** Read a monitor's EDID, the connector's override standing in for it as
 * Linux's drm_edid_read_ddc, and what it says, Linux's intel_dp_set_edid.
 * 1 when there is one. */
static int set_edid(struct intel_dp *dp)
{
    unset_edid(dp);
    if (!dp->connector->edid_override &&
        intel_dp_read_edid(dp, dp->edid, sizeof(dp->edid), &dp->edid_len) != 0) {
        dp->edid_len = 0;
    }
    drm_edid_hdmi_info(dp->connector, dp->edid, dp->edid_len, &dp->info);
    dp->edid_bpc = dp->info.bpc;
    update_dfp(dp);
    return dp->connector->edid_override || dp->edid_len != 0;
}

/** Read and answer the sink's device service requests, Linux's
 * intel_dp_check_device_service_irq; automated tests and content
 * protection are not served. */
static void check_device_service_irq(struct intel_dp *dp)
{
    uint8_t val;

    if (dp->dpcd[DP_DPCD_REV] < DP_DPCD_REV_11 ||
        intel_dp_dpcd_read(dp, DP_DEVICE_SERVICE_IRQ_VECTOR, &val, 1) != 1 || val == 0) {
        return;
    }
    (void)intel_dp_dpcd_writeb(dp, DP_DEVICE_SERVICE_IRQ_VECTOR, val);
    if (val & DP_SINK_SPECIFIC_IRQ) {
        klog(KLOG_DEBUG, "i915", "%s: sink specific irq unhandled", dp->name);
    }
}

/** Read and answer the sink's link service requests, Linux's
 * intel_dp_check_link_service_irq; an HDMI converter's link status change
 * is only acknowledged. */
static void check_link_service_irq(struct intel_dp *dp)
{
    uint8_t val;

    if (dp->dpcd[DP_DPCD_REV] < DP_DPCD_REV_11 ||
        intel_dp_dpcd_read(dp, DP_LINK_SERVICE_IRQ_VECTOR_ESI0, &val, 1) != 1 || val == 0) {
        return;
    }
    (void)intel_dp_dpcd_writeb(dp, DP_LINK_SERVICE_IRQ_VECTOR_ESI0, val);
}

/**
 * Whether the link of a trained monitor lost its training, Linux's
 * intel_dp_needs_link_retrain: the link status read, the link within its
 * bounds, and a failure in a row or channel equalization not done.
 */
static int needs_link_retrain(struct intel_dp *dp)
{
    uint32_t rate, lanes;
    int ok;

    if (!dp->link_trained) {
        return 0;
    }
    ok = intel_dp_link_ok(dp);
    if (ok < 0) {
        return 0;
    }
    link_max(dp, &rate, &lanes);
    if (dp->link_rate == 0 || dp->link_rate > rate || dp->lane_count == 0 ||
        dp->lane_count > lanes) {
        return 0;
    }
    return dp->train_failures != 0 || !ok;
}

/** Queue a retrain of a connected monitor whose link lost its training,
 * Linux's intel_dp_check_link_state; the hotplug work sets its pipe again. */
static void check_link_state(struct intel_dp *dp)
{
    if (dp->connector->status != DRM_CONNECTOR_CONNECTED || !needs_link_retrain(dp)) {
        return;
    }
    klog(KLOG_INFO, "i915", "%s: link retraining needed", dp->name);
    dp->retrain = 1;
    drm_dev_work_later(&dp->i915->drm, &dp->i915->display.hotplug.work);
}

/**
 * Probe a monitor, Linux's intel_dp_detect for an external port: no sink
 * while the hotplug pin is low, else its DPCD; then the link's bounds
 * forgotten after a long pulse, its training checked, and its EDID, which
 * makes a sink of unknown status connected.
 */
static int intel_dp_ext_detect(struct drm_connector *connector, int force_probe)
{
    struct intel_dp *dp = ext_dp(connector);
    int status;

    (void)force_probe;
    status = intel_dp_connected(dp) ? detect_dpcd(dp) : DRM_CONNECTOR_DISCONNECTED;
    if (status == DRM_CONNECTOR_DISCONNECTED) {
        unset_edid(dp);
        return status;
    }
    if (dp->reset_link) {
        dp->link_max_rate = 0;
        dp->link_max_lanes = 0;
        dp->train_failures = 0;
        dp->reset_link = 0;
    }
    check_link_state(dp);
    if (set_edid(dp)) {
        status = DRM_CONNECTOR_CONNECTED;
    }
    check_device_service_irq(dp);
    if (status != DRM_CONNECTOR_CONNECTED) {
        unset_edid(dp);
    }
    return status;
}

/** A forced connector's EDID, Linux's intel_dp_force: read when it is
 * forced on, forgotten when forced off. */
static void intel_dp_ext_force(struct drm_connector *connector)
{
    struct intel_dp *dp = ext_dp(connector);

    unset_edid(dp);
    if (connector->status == DRM_CONNECTOR_CONNECTED) {
        (void)set_edid(dp);
    }
}

/** The modes of a monitor's EDID, Linux's intel_dp_get_modes for an
 * external port; without one the core's modes stand in. */
static int intel_dp_ext_get_modes(struct drm_connector *connector)
{
    struct intel_dp *dp = ext_dp(connector);

    return drm_connector_update_edid(connector, dp->edid_len ? dp->edid : NULL, dp->edid_len);
}

/** A mode a monitor shows, Linux's intel_dp_mode_valid with
 * intel_dp_mode_valid_downstream: the transcoder's limits, no DBLCLK, a
 * clock from 10 MHz to the CD clock's, a link that carries it at 18 bits a
 * pixel, and a branch's port's dot clock and TMDS clock at 8 bits. */
static int32_t intel_dp_ext_mode_valid(struct drm_connector *connector,
                                       const struct drm_mode_modeinfo *mode)
{
    struct intel_dp *dp = ext_dp(connector);
    uint32_t rate, lanes;

    if (intel_cpu_transcoder_mode_valid(mode) != 0 || (mode->flags & DRM_MODE_FLAG_DBLCLK) ||
        mode->clock < 10000u || mode->clock > intel_max_dotclock(dp->i915)) {
        return -EINVAL;
    }
    if (dp->nrates == 0) {
        return -EINVAL;
    }
    link_max(dp, &rate, &lanes);
    if (link_required(mode->clock, 18u) > max_data_rate(rate, lanes)) {
        return -EINVAL;
    }
    if (dp->dfp_max_dotclock && mode->clock > dp->dfp_max_dotclock) {
        return -EINVAL;
    }
    return tmds_clock_valid(dp, mode->clock, 8, 1) ? 0 : -EINVAL;
}

static const struct drm_connector_funcs intel_dp_ext_funcs = {
    intel_dp_ext_detect, intel_dp_ext_get_modes, intel_dp_ext_mode_valid, intel_dp_ext_force};

int32_t intel_dp_ext_connector_init(struct drm_i915_private *i915, struct intel_ddi_port *p)
{
    static const char *const names[] = {"dp B", "dp C", "dp D"};
    const struct intel_bios_encoder *enc = intel_bios_encoder_lookup(&i915->vbt, p->port);
    struct intel_dp *dp = &p->dp;
    struct drm_connector *connector = &p->dp_connector;
    int32_t err;

    dp->i915 = i915;
    dp->name = names[p->port - INTEL_PORT_B];
    dp->port = p->port;
    dp->aux_port = enc && enc->aux_ch != INTEL_AUX_CH_NONE ? enc->aux_ch : p->port;
    dp->edp = 0;
    dp->connector = connector;
    /* Linux's defaults until a sink is read: 1.62 Gb/s on one lane. */
    dp->sink_rates[0] = 162000u;
    dp->nsink_rates = 1;
    dp->max_lanes = 1;
    set_common_rates(dp);
    if ((err = drm_connector_init(&i915->drm, connector, &intel_dp_ext_funcs,
                                  DRM_MODE_CONNECTOR_DisplayPort)) != 0) {
        return err;
    }
    /* Linux's intel_dp_init_connector on Haswell: interlace, no double
     * scan, and "max bpc" from 6 to 12. */
    connector->interlace_allowed = 1;
    connector->doublescan_allowed = 0;
    if ((err = drm_connector_attach_encoder(connector, &p->encoder)) != 0 ||
        (err = drm_connector_attach_max_bpc_property(connector, 6, 12)) != 0) {
        return err;
    }
    dp->present = 1;
    return 0;
}

int intel_dp_hpd_pulse(struct intel_dp *dp, int long_hpd)
{
    uint8_t old_sink_count = dp->sink_count;

    if (dp->edp) {
        return 0;
    }
    if (long_hpd) {
        dp->reset_link = 1;
        return 1;
    }
    /* A short pulse: a full probe when the sinks behind a branch changed or
     * the DPCD no longer reads. */
    if (!get_dpcd(dp) || old_sink_count != dp->sink_count) {
        return 1;
    }
    check_device_service_irq(dp);
    check_link_service_irq(dp);
    check_link_state(dp);
    return 0;
}
