/*
 * intel_dp.c: the eDP panel on DDI A, after Linux's display/intel_dp.c:
 * what the sink can do, read once at init, its connector, the link and
 * pipe configuration a mode takes, and the sink's power state.
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
static int32_t read_dpcd_caps(struct drm_i915_private *i915)
{
    struct intel_dp *dp = &i915->display.dp;
    uint8_t ext[DP_RECEIVER_CAP_SIZE];

    if (intel_dp_dpcd_read(i915, DP_DPCD_REV, dp->dpcd, DP_RECEIVER_CAP_SIZE) !=
            DP_RECEIVER_CAP_SIZE ||
        dp->dpcd[DP_DPCD_REV] == 0) {
        return -EIO;
    }
    if (!(dp->dpcd[DP_TRAINING_AUX_RD_INTERVAL] & DP_EXTENDED_RECEIVER_CAP_FIELD_PRESENT)) {
        return 0;
    }
    if (intel_dp_dpcd_read(i915, DP_DP13_DPCD_REV, ext, sizeof(ext)) != (int32_t)sizeof(ext)) {
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
static void init_source_oui(struct drm_i915_private *i915, int careful)
{
    uint8_t buf[3] = {0, 0, 0};

    if (careful) {
        if (intel_dp_dpcd_read(i915, DP_SOURCE_OUI, buf, sizeof(buf)) < 0) {
            klog(KLOG_ERR, "i915", "edp: failed to read source OUI");
        }
        if (buf[0] == intel_oui[0] && buf[1] == intel_oui[1] && buf[2] == intel_oui[2]) {
            return;
        }
    }
    if (intel_dp_dpcd_write(i915, DP_SOURCE_OUI, intel_oui, sizeof(intel_oui)) < 0) {
        klog(KLOG_ERR, "i915", "edp: failed to write source OUI");
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
        klog(KLOG_ERR, "i915", "edp: invalid DPCD with no link rates, using defaults");
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
static int32_t init_dpcd(struct drm_i915_private *i915)
{
    struct intel_dp *dp = &i915->display.dp;
    uint8_t table[2 * DP_MAX_SUPPORTED_RATES];
    uint32_t val;
    int i;

    if (read_dpcd_caps(i915) != 0) {
        return -EIO;
    }
    /* Read whatever DP_EDP_CONFIGURATION_CAP says, since some panels
     * leave its bit clear and still need eDP 1.4's detection; the
     * registers read 0 where they are missing. */
    if (intel_dp_dpcd_read(i915, DP_EDP_DPCD_REV, dp->edp_dpcd, EDP_DISPLAY_CTL_CAP_SIZE) ==
        EDP_DISPLAY_CTL_CAP_SIZE) {
        dp->use_max_params = dp->edp_dpcd[0] < DP_EDP_14;
    }
    dp->nsink_rates = 0;
    dp->use_rate_select = 0;
    if (dp->edp_dpcd[0] >= DP_EDP_14) {
        k_memset(table, 0, sizeof(table));
        (void)intel_dp_dpcd_read(i915, DP_SUPPORTED_LINK_RATES, table, sizeof(table));
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
    init_source_oui(i915, 1);
    return 0;
}

/** The fastest rate in kHz the VBT lets the port drive, Linux's
 * vbt_max_link_rate, 0 for no limit. */
static uint32_t vbt_max_link_rate(struct drm_i915_private *i915,
                                  const struct intel_bios_encoder *enc)
{
    uint32_t max = enc ? enc->dp_max_link_rate : 0, edp = i915->display.dp.vbt.edp.max_link_rate;

    if (max && edp) {
        return max < edp ? max : edp;
    }
    return max ? max : edp;
}

/** The rates both the port and the sink take, lowest first, Linux's
 * intel_dp_set_source_rates and intel_dp_set_common_rates. */
static void set_common_rates(struct drm_i915_private *i915, const struct intel_bios_encoder *enc)
{
    struct intel_dp *dp = &i915->display.dp;
    uint32_t max = vbt_max_link_rate(i915, enc);
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
 * intel_dp_max_common_lane_count: DDI A has 4 only when the firmware gave
 * it DDI E's, and the VBT may lower it. */
static uint32_t max_common_lanes(struct drm_i915_private *i915)
{
    struct intel_dp *dp = &i915->display.dp;
    const struct intel_bios_encoder *enc = intel_bios_encoder_lookup(&i915->vbt, INTEL_PORT_A);
    uint32_t source = (dp->saved_port_bits & DDI_A_4_LANES) ? 4u : 2u;

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

    /* Linux's intel_setup_outputs on Haswell: DDI A is set up when the
     * strap says a display is there, and it is always eDP. */
    if (!(buf & DDI_INIT_DISPLAY_DETECTED)) {
        klog(KLOG_INFO, "i915", "edp: no display detected on DDI A");
        return -ENODEV;
    }
    dp->port = INTEL_PORT_A;
    dp->saved_port_bits = buf & (DDI_BUF_PORT_REVERSAL | DDI_A_4_LANES);
    dp->vbt.panel_type = -1;
    intel_bios_panel_init(&i915->vbt, &dp->vbt, enc, NULL, 0);
    intel_pps_init(i915);
    if (init_dpcd(i915) != 0) {
        klog(KLOG_INFO, "i915", "edp: failed to retrieve link info, disabling eDP");
        intel_pps_vdd_off_unlocked(i915, 1);
        return -ENODEV;
    }
    if (intel_dp_read_edid(i915, dp->edid, sizeof(dp->edid), &dp->edid_len) != 0) {
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
    set_common_rates(i915, enc);
    dp->edid_bpc = edid_bpc(dp);
    dp->present = 1;
    klog(KLOG_INFO, "i915",
         "edp: DPCD %x.%x, eDP %u, %u lane%s, rates %u to %u kHz%s, %u bpc, EDID %u bytes",
         (unsigned)(dp->dpcd[DP_DPCD_REV] >> 4), (unsigned)(dp->dpcd[DP_DPCD_REV] & 0xfu),
         (unsigned)dp->edp_dpcd[0], (unsigned)max_common_lanes(i915),
         max_common_lanes(i915) == 1 ? "" : "s", (unsigned)dp->rates[0],
         (unsigned)dp->rates[dp->nrates - 1], dp->use_rate_select ? " by index" : "",
         (unsigned)dp->edid_bpc, (unsigned)dp->edid_len);
    return 0;
}

/* ------------------------------------------------------------------------
 * The sink's power
 */

void intel_dp_set_power(struct drm_i915_private *i915, uint8_t mode)
{
    struct intel_dp *dp = &i915->display.dp;
    int32_t ret = 0;
    int i;

    if (dp->dpcd[DP_DPCD_REV] < DP_DPCD_REV_11) {
        return;
    }
    /* An eDP panel has no downstream port that needs D0 for its HPD. */
    if (mode != DP_SET_POWER_D0) {
        ret = intel_dp_dpcd_writeb(i915, DP_SET_POWER, mode);
    } else {
        init_source_oui(i915, 0);
        /* The sink may take a millisecond to wake. */
        for (i = 0; i < 3; i++) {
            ret = intel_dp_dpcd_writeb(i915, DP_SET_POWER, mode);
            if (ret == 1) {
                break;
            }
            intel_display_msleep(i915, 1);
        }
    }
    if (ret != 1) {
        klog(KLOG_WARN, "i915", "edp: failed to %s sink power state",
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
 * to the panel's colour depth, 8 bits when its EDID says none, and to "max
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
 * the sink's most down to 18 in steps of 6, then the slowest rate, then the
 * fewest lanes that carry it. A panel that failed to train at the least
 * takes its most (`use_max_params`), and a fallback bounds the link.
 * 0, or -EINVAL when no link carries the mode.
 */
static int32_t compute_link(struct drm_i915_private *i915, const struct drm_connector_state *conn,
                            struct intel_crtc_config *cfg)
{
    struct intel_dp *dp = &i915->display.dp;
    uint32_t clock = cfg->mode.clock, max_rate, min_rate, max_lanes, min_lanes, lanes, bpp;
    int i;

    max_rate = dp->link_max_rate ? dp->link_max_rate : dp->rates[dp->nrates - 1];
    max_lanes = dp->link_max_lanes ? dp->link_max_lanes : max_common_lanes(i915);
    min_rate = dp->rates[0] < max_rate ? dp->rates[0] : max_rate;
    min_lanes = 1;
    if (dp->use_max_params) {
        min_rate = max_rate;
        min_lanes = max_lanes;
    }
    bpp = sink_pipe_bpp(dp, conn);
    /* Linux's intel_dp_max_bpp: the VBT's depth only for a panel whose
     * EDID gives none. */
    if (dp->edid_bpc == 0 && dp->vbt.edp.bpp && dp->vbt.edp.bpp < bpp) {
        bpp = dp->vbt.edp.bpp;
    }
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

int32_t intel_dp_compute_config(struct drm_i915_private *i915, int pipe,
                                const struct drm_connector_state *conn, int crc_wa,
                                struct intel_crtc_config *cfg)
{
    const struct drm_mode_modeinfo *m = &cfg->mode;
    uint32_t data_m;
    int32_t err;

    err = intel_panel_compute_config(i915, &cfg->mode);
    if (err != 0) {
        return err;
    }
    /* eDP takes no interlace; DBLSCAN and DBLCLK nobody. */
    if (m->flags & (DRM_MODE_FLAG_DBLSCAN | DRM_MODE_FLAG_INTERLACE | DRM_MODE_FLAG_DBLCLK)) {
        return -EINVAL;
    }
    err = compute_link(i915, conn, cfg);
    if (err != 0) {
        return err;
    }
    err = intel_pfit_compute_config(pipe, conn->scaling_mode, cfg);
    if (err != 0) {
        return err;
    }
    cfg->dither = cfg->bpp == 18u;
    /* Linux's intel_link_compute_m_n with the fixed N of Windows and the
     * firmware, and a transfer unit of 64. */
    data_m = link_required(m->clock, cfg->bpp);
    cfg->tu = 64;
    compute_m_n(&cfg->data_m, &cfg->data_n, data_m, cfg->port_clock * cfg->lanes, 0x8000000u);
    compute_m_n(&cfg->link_m, &cfg->link_n, m->clock, cfg->port_clock, 0x80000u);
    /* Pipe A drives the eDP transcoder through the fitter's power well
     * while the fitter or a CRC of it runs (Linux's force_thru). */
    cfg->force_thru = pipe == I915_PIPE_A && (cfg->pfit || crc_wa);
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

/** Linux's intel_cpu_transcoder_mode_valid: a transcoder's least blanking,
 * and no hsync at the end of the active area. */
static int32_t transcoder_mode_valid(const struct drm_mode_modeinfo *m)
{
    if (m->hdisplay < 64 || m->htotal < m->hdisplay + 32u || m->vtotal < m->vdisplay + 5u ||
        m->hsync_start == m->hdisplay) {
        return -EINVAL;
    }
    return 0;
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

    if (transcoder_mode_valid(mode) != 0 || (mode->flags & DRM_MODE_FLAG_DBLCLK) ||
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
    rate = dp->link_max_rate ? dp->link_max_rate : dp->rates[dp->nrates - 1];
    lanes = dp->link_max_lanes ? dp->link_max_lanes : max_common_lanes(i915);
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
     * from 6 to 12, and "scaling mode" without None, Full aspect first. */
    if ((err = drm_connector_attach_max_bpc_property(&d->connector, 6, 12)) != 0 ||
        (err = drm_connector_attach_scaling_mode_property(
             &d->connector, 1u << DRM_MODE_SCALE_ASPECT | 1u << DRM_MODE_SCALE_FULLSCREEN |
                                1u << DRM_MODE_SCALE_CENTER)) != 0) {
        return err;
    }
    d->connector.state->scaling_mode = DRM_MODE_SCALE_ASPECT;
    return 0;
}
