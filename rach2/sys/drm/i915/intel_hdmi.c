/*
 * intel_hdmi.c: HDMI and DVI monitors on Haswell's DDI ports B to D, after
 * Linux's display/intel_hdmi.c and drm_dp_dual_mode_helper.c: the sink's
 * EDID over GMBUS, a DP++ adaptor's registers, the connector, the bits a
 * colour and TMDS clock a mode takes, and the infoframes.
 */

#include "i915.h"
#include "i915_reg.h"

#include "../../klog.h"
#include "../../lib/kerrno.h"
#include "../../lib/kstring.h"

/* Haswell's most TMDS clock in kHz (Linux's
 * intel_hdmi_source_max_tmds_clock), and the least HDMI takes. */
#define HSW_MAX_TMDS  300000u
#define HDMI_MIN_TMDS 25000u

/* A DP++ adaptor's registers on the DDC bus (Linux's
 * drm_dp_dual_mode_helper.h). */
#define DP_DUAL_MODE_SLAVE_ADDRESS  0x40
#define DP_DUAL_MODE_HDMI_ID        0x00
#define DP_DUAL_MODE_HDMI_ID_LEN    16
#define DP_DUAL_MODE_ADAPTOR_ID     0x10
#define DP_DUAL_MODE_TYPE_TYPE2     0xa0
#define DP_DUAL_MODE_REV_TYPE2      0x00
#define DP_DUAL_MODE_TYPE_HAS_DPCD  0x08
#define DP_DUAL_MODE_MAX_TMDS_CLOCK 0x1d
#define DP_DUAL_MODE_TMDS_OEN       0x20
#define DP_DUAL_MODE_TMDS_DISABLE   0x01

/* The source type Linux's SPD infoframe names, a PC. */
#define HDMI_SPD_SDI_PC 0x09

/** The DDI port of the HDMI connector `connector`. */
static struct intel_ddi_port *hdmi_port(struct drm_connector *connector)
{
    struct drm_i915_private *i915 = to_i915(connector->dev);
    int i;

    for (i = 0; i < 3; i++) {
        if (connector == &i915->display.ddi[i].hdmi_connector) {
            return &i915->display.ddi[i];
        }
    }
    return &i915->display.ddi[0];
}

/* ------------------------------------------------------------------------
 * DP++ adaptors
 */

/**
 * Read `size` bytes at `offset` of a DP++ adaptor, Linux's
 * drm_dp_dual_mode_read: always from offset 0, since not every adaptor
 * takes an offset, the bytes before `offset` dropped. 0, or the bus's
 * error.
 */
static int32_t dual_mode_read(const struct intel_i2c_bus *bus, uint8_t offset, uint8_t *buf,
                              uint32_t size)
{
    uint8_t zero = 0, tmp[DP_DUAL_MODE_TMDS_OEN + 1];
    struct intel_i2c_msg msgs[2];
    int32_t ret;

    msgs[0].addr = DP_DUAL_MODE_SLAVE_ADDRESS;
    msgs[0].read = 0;
    msgs[0].len = 1;
    msgs[0].buf = &zero;
    msgs[1].addr = DP_DUAL_MODE_SLAVE_ADDRESS;
    msgs[1].read = 1;
    msgs[1].len = size + offset;
    msgs[1].buf = tmp;
    ret = bus->xfer(bus->ctx, msgs, 2);
    if (ret < 0) {
        return ret;
    }
    if (ret != 2) {
        return -EPROTO;
    }
    k_memcpy(buf, tmp + offset, size);
    return 0;
}

/** Write one byte at `offset` of a DP++ adaptor, Linux's
 * drm_dp_dual_mode_write. 0, or the bus's error. */
static int32_t dual_mode_write(const struct intel_i2c_bus *bus, uint8_t offset, uint8_t value)
{
    uint8_t data[2];
    struct intel_i2c_msg msg;
    int32_t ret;

    data[0] = offset;
    data[1] = value;
    msg.addr = DP_DUAL_MODE_SLAVE_ADDRESS;
    msg.read = 0;
    msg.len = 2;
    msg.buf = data;
    ret = bus->xfer(bus->ctx, &msg, 1);
    if (ret < 0) {
        return ret;
    }
    return ret == 1 ? 0 : -EPROTO;
}

/** Whether `id` is the HDMI adaptor's identifier. */
static int is_hdmi_adaptor(const uint8_t *id)
{
    static const char want[] = "DP-HDMI ADAPTOR\x04";
    int i;

    for (i = 0; i < DP_DUAL_MODE_HDMI_ID_LEN; i++) {
        if (id[i] != (uint8_t)want[i]) {
            return 0;
        }
    }
    return 1;
}

/**
 * The kind of DP++ adaptor on `bus`, Linux's drm_dp_dual_mode_detect:
 * unknown when its HDMI identifier does not read, an LSPCON or a type 2
 * adaptor by its adaptor identifier, and type 1 otherwise, DVI or HDMI by
 * the identifier.
 */
static int dual_mode_detect(const struct intel_i2c_bus *bus)
{
    uint8_t hdmi_id[DP_DUAL_MODE_HDMI_ID_LEN], adaptor_id = 0;

    k_memset(hdmi_id, 0, sizeof(hdmi_id));
    if (dual_mode_read(bus, DP_DUAL_MODE_HDMI_ID, hdmi_id, sizeof(hdmi_id)) != 0) {
        return DP_DUAL_MODE_UNKNOWN;
    }
    if (dual_mode_read(bus, DP_DUAL_MODE_ADAPTOR_ID, &adaptor_id, 1) == 0) {
        if (is_hdmi_adaptor(hdmi_id) &&
            adaptor_id == (DP_DUAL_MODE_TYPE_TYPE2 | DP_DUAL_MODE_TYPE_HAS_DPCD)) {
            return DP_DUAL_MODE_LSPCON;
        }
        if (adaptor_id == (DP_DUAL_MODE_TYPE_TYPE2 | DP_DUAL_MODE_REV_TYPE2)) {
            return is_hdmi_adaptor(hdmi_id) ? DP_DUAL_MODE_TYPE2_HDMI : DP_DUAL_MODE_TYPE2_DVI;
        }
        if (adaptor_id != 0 && adaptor_id != 0xff) {
            klog(KLOG_ERR, "i915", "%s: unexpected DP dual mode adaptor ID %02x", bus->name,
                 (unsigned)adaptor_id);
        }
    }
    return is_hdmi_adaptor(hdmi_id) ? DP_DUAL_MODE_TYPE1_HDMI : DP_DUAL_MODE_TYPE1_DVI;
}

/** The most TMDS clock in kHz an adaptor of `type` carries, Linux's
 * drm_dp_dual_mode_max_tmds_clock: 165 MHz for type 1, a type 2's own,
 * and no limit, 0, for none. */
static uint32_t dual_mode_max_tmds(const struct intel_i2c_bus *bus, int type)
{
    uint8_t max = 0;

    if (type == DP_DUAL_MODE_NONE) {
        return 0;
    }
    if (type < DP_DUAL_MODE_TYPE2_DVI) {
        return 165000u;
    }
    if (dual_mode_read(bus, DP_DUAL_MODE_MAX_TMDS_CLOCK, &max, 1) != 0 || max == 0 || max == 0xff) {
        klog(KLOG_DEBUG, "i915", "%s: failed to query max TMDS clock", bus->name);
        return 165000u;
    }
    return max * 5000u / 2u;
}

void intel_hdmi_dual_mode_tmds(struct intel_ddi_port *p, int enable)
{
    struct intel_i2c_bus *bus = intel_gmbus_bus(p->dp.i915, p->hdmi.ddc_pin);
    uint8_t oen = enable ? 0 : DP_DUAL_MODE_TMDS_DISABLE, tmp;
    int retry;

    if (p->hdmi.adaptor < DP_DUAL_MODE_TYPE2_DVI || !bus) {
        return;
    }
    /* An LSPCON asleep may miss the first write; read it back. */
    for (retry = 0; retry < 3; retry++) {
        if (dual_mode_write(bus, DP_DUAL_MODE_TMDS_OEN, oen) != 0 ||
            dual_mode_read(bus, DP_DUAL_MODE_TMDS_OEN, &tmp, 1) != 0) {
            klog(KLOG_DEBUG, "i915", "%s: failed to %s TMDS output buffers", bus->name,
                 enable ? "enable" : "disable");
            return;
        }
        if (tmp == oen) {
            return;
        }
    }
    klog(KLOG_DEBUG, "i915", "%s: I2C write value mismatch during TMDS output buffer %s", bus->name,
         enable ? "enable" : "disable");
}

/* ------------------------------------------------------------------------
 * The sink
 */

/** Forget the sink's EDID and adaptor, Linux's intel_hdmi_unset_edid. */
static void hdmi_unset_edid(struct intel_hdmi *hdmi)
{
    hdmi->adaptor = DP_DUAL_MODE_NONE;
    hdmi->adaptor_max_tmds = 0;
    hdmi->edid_len = 0;
    k_memset(&hdmi->info, 0, sizeof(hdmi->info));
}

/**
 * Find a DP++ adaptor, Linux's intel_hdmi_dp_dual_mode_detect: an adaptor
 * that does not answer is a type 1 DVI one when the VBT says the port is
 * DP++ and the connector is not forced, else none; and the most clock of
 * one on a port the VBT does not call DP++ is not trusted.
 */
static void dual_mode_probe(struct intel_ddi_port *p, const struct intel_i2c_bus *bus)
{
    struct intel_hdmi *hdmi = &p->hdmi;
    int type = dual_mode_detect(bus);

    if (type == DP_DUAL_MODE_UNKNOWN) {
        type = !p->hdmi_connector.force && hdmi->dual_mode ? DP_DUAL_MODE_TYPE1_DVI
                                                           : DP_DUAL_MODE_NONE;
    }
    if (type == DP_DUAL_MODE_NONE) {
        return;
    }
    hdmi->adaptor = type;
    hdmi->adaptor_max_tmds = dual_mode_max_tmds(bus, type);
    if (!hdmi->dual_mode) {
        hdmi->adaptor_max_tmds = 0;
    }
}

/**
 * Read the sink's EDID, the connector's override standing in for it, over
 * GMBUS and again by bit-banging when GMBUS fails, and what it says,
 * Linux's intel_hdmi_set_edid. 1 for a digital sink, which is there.
 */
static int hdmi_set_edid(struct intel_ddi_port *p)
{
    struct drm_i915_private *i915 = p->dp.i915;
    struct intel_hdmi *hdmi = &p->hdmi;
    struct drm_connector *connector = &p->hdmi_connector;
    struct intel_i2c_bus *bus = intel_gmbus_bus(i915, hdmi->ddc_pin);
    uint32_t len = 0;
    int32_t err;

    hdmi_unset_edid(hdmi);
    if (!connector->edid_override && bus) {
        err = intel_edid_read(bus, hdmi->edid, sizeof(hdmi->edid), &len);
        if (err != 0 && !intel_gmbus_is_forced_bit(i915, hdmi->ddc_pin)) {
            klog(KLOG_DEBUG, "i915",
                 "%s: HDMI GMBUS EDID read failed, retry using GPIO bit-banging", bus->name);
            intel_gmbus_force_bit(i915, hdmi->ddc_pin, 1);
            err = intel_edid_read(bus, hdmi->edid, sizeof(hdmi->edid), &len);
            intel_gmbus_force_bit(i915, hdmi->ddc_pin, 0);
        }
        hdmi->edid_len = err == 0 ? len : 0;
    }
    drm_edid_hdmi_info(connector, hdmi->edid, hdmi->edid_len, &hdmi->info);
    if (!hdmi->info.digital) {
        return 0;
    }
    if (bus) {
        dual_mode_probe(p, bus);
    }
    return 1;
}

/** Probe the sink, Linux's intel_hdmi_detect on Haswell: by its EDID
 * alone, since the port's live status is not trusted before Gen 11. */
static int intel_hdmi_detect(struct drm_connector *connector, int force_probe)
{
    struct drm_i915_private *i915 = to_i915(connector->dev);
    int status;

    (void)force_probe;
    /* Linux's intel_hdmi_detect holds the GMBUS domain's wakeref. */
    i915_rpm_get(i915);
    status =
        hdmi_set_edid(hdmi_port(connector)) ? DRM_CONNECTOR_CONNECTED : DRM_CONNECTOR_DISCONNECTED;
    i915_rpm_put(i915);
    return status;
}

/** A forced connector's EDID, Linux's intel_hdmi_force: read when it is
 * forced on, forgotten when forced off. */
static void intel_hdmi_force(struct drm_connector *connector)
{
    struct intel_ddi_port *p = hdmi_port(connector);

    hdmi_unset_edid(&p->hdmi);
    if (connector->status == DRM_CONNECTOR_CONNECTED) {
        (void)hdmi_set_edid(p);
    }
}

/** The modes of the sink's EDID, Linux's intel_hdmi_get_modes. */
static int intel_hdmi_get_modes(struct drm_connector *connector)
{
    struct intel_hdmi *hdmi = &hdmi_port(connector)->hdmi;

    return drm_connector_update_edid(connector, hdmi->edid_len ? hdmi->edid : NULL, hdmi->edid_len);
}

/* ------------------------------------------------------------------------
 * Clocks and colour depth
 */

/** Linux's intel_hdmi_tmds_clock for RGB: the TMDS clock in kHz of a mode
 * of `clock` kHz at `bpc` bits a colour. */
static uint32_t hdmi_tmds_clock(uint32_t clock, uint32_t bpc)
{
    return (clock * bpc + 4u) / 8u;
}

/** The most TMDS clock in kHz the port drives to its sink, Linux's
 * hdmi_port_clock_limit: the source's, and with `respect` the adaptor's,
 * the sink's or DVI's 165 MHz. */
static uint32_t port_clock_limit(const struct intel_hdmi *hdmi, int respect, int has_hdmi_sink)
{
    uint32_t max = hdmi->max_tmds;

    if (respect) {
        if (hdmi->adaptor_max_tmds && hdmi->adaptor_max_tmds < max) {
            max = hdmi->adaptor_max_tmds;
        }
        if (hdmi->info.max_tmds_clock) {
            max = hdmi->info.max_tmds_clock < max ? hdmi->info.max_tmds_clock : max;
        } else if (!has_hdmi_sink && max > 165000u) {
            max = 165000u;
        }
    }
    return max;
}

/** Whether the port carries a TMDS clock of `clock` kHz, Linux's
 * hdmi_port_clock_valid for Haswell. */
static int port_clock_valid(const struct intel_hdmi *hdmi, uint32_t clock, int respect,
                            int has_hdmi_sink)
{
    return clock >= HDMI_MIN_TMDS && clock <= port_clock_limit(hdmi, respect, has_hdmi_sink);
}

/** Whether source and sink take `bpc` bits a colour, Linux's
 * intel_hdmi_source_bpc_possible and intel_hdmi_sink_bpc_possible:
 * Haswell drives 8 and 12, and 12 goes to an HDMI sink with deep colour. */
static int bpc_possible(const struct intel_hdmi *hdmi, int bpc, int has_hdmi_sink)
{
    switch (bpc) {
    case 12:
        return has_hdmi_sink && hdmi->info.dc_36;
    case 8:
        return 1;
    default:
        return 0;
    }
}

/** A mode the sink shows, Linux's intel_hdmi_mode_valid for RGB: the
 * transcoder's limits, a clock within the CD clock's, twice the mode's for
 * a frame-packed stereo mode, DBLCLK for an HDMI sink at twice the clock,
 * and some depth whose TMDS clock the port, the adaptor and the sink
 * carry. */
static int32_t intel_hdmi_mode_valid(struct drm_connector *connector,
                                     const struct drm_mode_modeinfo *mode)
{
    struct intel_ddi_port *p = hdmi_port(connector);
    int has_hdmi_sink = p->hdmi.info.is_hdmi, bpc;
    uint32_t clock = mode->clock;

    if (intel_cpu_transcoder_mode_valid(mode) != 0) {
        return -EINVAL;
    }
    if ((mode->flags & DRM_MODE_FLAG_3D_MASK) == DRM_MODE_FLAG_3D_FRAME_PACKING) {
        clock *= 2u;
    }
    if (clock > intel_max_dotclock(p->dp.i915)) {
        return -EINVAL;
    }
    if (mode->flags & DRM_MODE_FLAG_DBLCLK) {
        if (!has_hdmi_sink) {
            return -EINVAL;
        }
        clock *= 2u;
    }
    for (bpc = 12; bpc >= 8; bpc -= 2) {
        if (bpc_possible(&p->hdmi, bpc, has_hdmi_sink) &&
            port_clock_valid(&p->hdmi, hdmi_tmds_clock(clock, (uint32_t)bpc), 1, has_hdmi_sink)) {
            return 0;
        }
    }
    return -EINVAL;
}

static const struct drm_connector_funcs intel_hdmi_funcs = {
    intel_hdmi_detect, intel_hdmi_get_modes, intel_hdmi_mode_valid, intel_hdmi_force};

int32_t intel_hdmi_connector_init(struct drm_i915_private *i915, struct intel_ddi_port *p)
{
    static const uint8_t default_pin[] = {GMBUS_PIN_DPB, GMBUS_PIN_DPC, GMBUS_PIN_DPD};
    const struct intel_bios_encoder *enc = intel_bios_encoder_lookup(&i915->vbt, p->port);
    struct intel_hdmi *hdmi = &p->hdmi;
    struct drm_connector *connector = &p->hdmi_connector;
    int32_t err;

    /* Linux's intel_hdmi_ddc_pin, intel_ddi_hdmi_level and
     * intel_hdmi_source_max_tmds_clock from the VBT. */
    hdmi->ddc_pin =
        enc && intel_gmbus_is_valid_pin(enc->ddc_pin) ? enc->ddc_pin : default_pin[p->port - 1];
    hdmi->level =
        enc && enc->hdmi_level_shift >= 0 ? enc->hdmi_level_shift : intel_ddi_hdmi_default_level();
    hdmi->max_tmds = HSW_MAX_TMDS;
    if (enc && enc->hdmi_max_tmds_clock && enc->hdmi_max_tmds_clock < hdmi->max_tmds) {
        hdmi->max_tmds = enc->hdmi_max_tmds_clock;
    }
    hdmi->dual_mode = enc && intel_bios_encoder_supports_dp_dual_mode(enc);
    hdmi_unset_edid(hdmi);
    if ((err = drm_connector_init(&i915->drm, connector, &intel_hdmi_funcs,
                                  DRM_MODE_CONNECTOR_HDMIA)) != 0) {
        return err;
    }
    connector->interlace_allowed = 1;
    connector->doublescan_allowed = 0;
    connector->stereo_allowed = 1;
    if ((err = drm_connector_attach_encoder(connector, &p->encoder)) != 0 ||
        (err = drm_connector_attach_max_bpc_property(connector, 8, 12)) != 0) {
        return err;
    }
    klog(KLOG_INFO, "i915", "hdmi %c: DDC pin %u, level %d, %u kHz TMDS%s", 'A' + p->port,
         (unsigned)hdmi->ddc_pin, hdmi->level, (unsigned)hdmi->max_tmds,
         hdmi->dual_mode ? ", DP++" : "");
    return 0;
}

/* ------------------------------------------------------------------------
 * A mode's configuration
 */

/** The pipe's bits a pixel the connector allows, Linux's
 * compute_baseline_pipe_bpp and compute_sink_pipe_bpp. */
static uint32_t hdmi_sink_pipe_bpp(const struct intel_hdmi *hdmi,
                                   const struct drm_connector_state *conn)
{
    uint32_t bpc = hdmi->info.bpc ? hdmi->info.bpc : 8u;

    if (conn->max_requested_bpc && conn->max_requested_bpc < bpc) {
        bpc = conn->max_requested_bpc;
    }
    bpc = bpc < 8 ? 6 : bpc < 10 ? 8 : bpc < 12 ? 10 : 12;
    return 3u * bpc;
}

/** Choose the bits a colour and the TMDS clock of `cfg`'s mode, Linux's
 * intel_hdmi_compute_clock and intel_hdmi_compute_bpc: from the pipe's
 * depth down in steps of 2 to 8, 8 alone when the limits are not kept.
 * 0, or -EINVAL. */
static int32_t compute_clock(const struct intel_hdmi *hdmi, struct intel_crtc_config *cfg,
                             uint32_t pipe_bpp, int respect)
{
    uint32_t clock = cfg->mode.clock * cfg->pixel_multiplier;
    int bpc = (int)pipe_bpp / 3;

    if (bpc < 8 || !respect) {
        bpc = 8;
    }
    for (; bpc >= 8; bpc -= 2) {
        uint32_t tmds = hdmi_tmds_clock(clock, (uint32_t)bpc);

        if (bpc_possible(hdmi, bpc, cfg->has_hdmi_sink) &&
            port_clock_valid(hdmi, tmds, respect, cfg->has_hdmi_sink)) {
            cfg->port_clock = tmds;
            cfg->bpp = pipe_bpp < 3u * (uint32_t)bpc ? pipe_bpp : 3u * (uint32_t)bpc;
            return 0;
        }
    }
    return -EINVAL;
}

/** Whether a deep colour mode's horizontal timings fall on whole pixel
 * groups, so that GCP may say the default phase, Linux's
 * gcp_default_phase_possible. */
static int gcp_default_phase_possible(uint32_t bpp, const struct drm_mode_modeinfo *m)
{
    uint32_t n;

    switch (bpp) {
    case 30:
        n = 4;
        break;
    case 36:
        n = 2;
        break;
    case 48:
        n = 1;
        break;
    default:
        return 0;
    }
    return m->hdisplay % n == 0 && m->htotal % n == 0 && m->hsync_start % n == 0 &&
           m->hsync_end % n == 0 &&
           (!(m->flags & DRM_MODE_FLAG_INTERLACE) || (m->htotal / 2u) % n == 0);
}

/** Pack an infoframe of `len` bytes from `frame` into the DIP's layout in
 * `out`, Linux's intel_write_infoframe: a hole after the three bytes of
 * the header; the bytes, one more. */
static uint32_t dip_pack(const uint8_t *frame, uint32_t len, uint8_t *out)
{
    k_memset(out, 0, INTEL_DIP_DATA_SIZE);
    if (len == 0) {
        return 0;
    }
    k_memcpy(out, frame, 3);
    k_memcpy(out + 4, frame + 3, len - 3);
    return len + 1;
}

int32_t intel_hdmi_compute_config(struct intel_ddi_port *p, int pipe,
                                  const struct drm_connector_state *conn,
                                  const struct drm_mode_modeinfo *mode,
                                  struct intel_crtc_config *cfg)
{
    struct intel_hdmi *hdmi = &p->hdmi;
    const struct drm_mode_modeinfo *m = mode;
    uint8_t frame[DRM_INFOFRAME_MAX];
    uint32_t pipe_bpp, len;
    int32_t err;

    if ((m->flags & DRM_MODE_FLAG_DBLSCAN) ||
        ((m->flags & DRM_MODE_FLAG_INTERLACE) && !p->hdmi_connector.interlace_allowed)) {
        return -EINVAL;
    }
    cfg->output = INTEL_OUTPUT_HDMI;
    cfg->port = p->port;
    cfg->trans = pipe;
    cfg->has_hdmi_sink = hdmi->info.is_hdmi != 0;
    cfg->pixel_multiplier = (m->flags & DRM_MODE_FLAG_DBLCLK) ? 2u : 1u;
    /* The sink's limits first; then on the chance the user knows
     * better. */
    pipe_bpp = hdmi_sink_pipe_bpp(hdmi, conn);
    err = compute_clock(hdmi, cfg, pipe_bpp, 1);
    if (err != 0) {
        err = compute_clock(hdmi, cfg, pipe_bpp, 0);
    }
    if (err != 0) {
        klog(KLOG_DEBUG, "i915", "hdmi %c: unsupported HDMI clock (%u kHz), rejecting mode",
             'A' + p->port, (unsigned)cfg->mode.clock);
        return err;
    }
    /* Linux's intel_hdmi_limited_color_range with "Broadcast RGB" on
     * automatic. */
    cfg->limited_range = cfg->has_hdmi_sink && drm_default_rgb_quant_limited(m);
    cfg->lanes = 4;
    cfg->infoframes = 0;
    cfg->gcp = 0;
    cfg->avi_len = cfg->spd_len = cfg->vs_len = 0;
    if (!cfg->has_hdmi_sink) {
        return 0;
    }
    /* Linux's intel_hdmi_compute_gcp_infoframe, _avi_, _spd_ and
     * _hdmi_infoframe. */
    cfg->infoframes |= VIDEO_DIP_ENABLE_GCP_HSW;
    if (cfg->bpp > 24u) {
        cfg->gcp |= GCP_COLOR_INDICATION;
    }
    if (gcp_default_phase_possible(cfg->bpp, m)) {
        cfg->gcp |= GCP_DEFAULT_PHASE_ENABLE;
    }
    len = drm_hdmi_avi_infoframe(&hdmi->info, m, cfg->limited_range, frame);
    if (len == 0) {
        klog(KLOG_DEBUG, "i915", "hdmi %c: bad AVI infoframe", 'A' + p->port);
        return -EINVAL;
    }
    cfg->infoframes |= VIDEO_DIP_ENABLE_AVI_HSW;
    cfg->avi_len = dip_pack(frame, len, cfg->avi);
    len = drm_hdmi_spd_infoframe("Intel", "Integrated gfx", HDMI_SPD_SDI_PC, frame);
    cfg->infoframes |= VIDEO_DIP_ENABLE_SPD_HSW;
    cfg->spd_len = dip_pack(frame, len, cfg->spd);
    len = drm_hdmi_vendor_infoframe(&hdmi->info, m, frame);
    if (len != 0) {
        cfg->infoframes |= VIDEO_DIP_ENABLE_VS_HSW;
        cfg->vs_len = dip_pack(frame, len, cfg->vs);
    }
    return 0;
}

/* ------------------------------------------------------------------------
 * Infoframes
 */

/** Write one infoframe of `len` DIP bytes into the data registers from
 * `data` and enable it, Linux's hsw_write_infoframe: its enable off, the
 * bytes and zeroes to the end of the DIP for the ECC, and the enable on. */
static void write_infoframe(struct drm_i915_private *i915, int trans, uint32_t enable,
                            uint32_t data_reg, const uint8_t *data, uint32_t len)
{
    struct i915_hw *hw = &i915->hw;
    uint32_t ctl = HSW_TVIDEO_DIP_CTL(trans), val = i915_read(hw, ctl), i, dw;

    i915_write(hw, ctl, val & ~enable);
    for (i = 0; i < INTEL_DIP_DATA_SIZE; i += 4) {
        dw = i < len ? (uint32_t)data[i] | (uint32_t)data[i + 1] << 8 |
                           (uint32_t)data[i + 2] << 16 | (uint32_t)data[i + 3] << 24
                     : 0;
        i915_write(hw, data_reg + i, dw);
    }
    i915_write(hw, ctl, val | enable);
    i915_posting_read(hw, ctl);
}

void intel_hdmi_set_infoframes(struct drm_i915_private *i915, int enable,
                               const struct intel_crtc_config *cfg)
{
    struct i915_hw *hw = &i915->hw;
    uint32_t reg = HSW_TVIDEO_DIP_CTL(cfg->trans), val = i915_read(hw, reg);

    val &= ~(VIDEO_DIP_ENABLE_VSC_HSW | VIDEO_DIP_ENABLE_AVI_HSW | VIDEO_DIP_ENABLE_GCP_HSW |
             VIDEO_DIP_ENABLE_VS_HSW | VIDEO_DIP_ENABLE_GMP_HSW | VIDEO_DIP_ENABLE_SPD_HSW |
             VIDEO_DIP_ENABLE_DRM_GLK);
    if (!enable || !cfg->has_hdmi_sink) {
        i915_write(hw, reg, val);
        i915_posting_read(hw, reg);
        return;
    }
    if (cfg->infoframes & VIDEO_DIP_ENABLE_GCP_HSW) {
        i915_write(hw, HSW_TVIDEO_DIP_GCP(cfg->trans), cfg->gcp);
        val |= VIDEO_DIP_ENABLE_GCP_HSW;
    }
    i915_write(hw, reg, val);
    i915_posting_read(hw, reg);
    if (cfg->infoframes & VIDEO_DIP_ENABLE_AVI_HSW) {
        write_infoframe(i915, cfg->trans, VIDEO_DIP_ENABLE_AVI_HSW,
                        HSW_TVIDEO_DIP_AVI_DATA(cfg->trans), cfg->avi, cfg->avi_len);
    }
    if (cfg->infoframes & VIDEO_DIP_ENABLE_SPD_HSW) {
        write_infoframe(i915, cfg->trans, VIDEO_DIP_ENABLE_SPD_HSW,
                        HSW_TVIDEO_DIP_SPD_DATA(cfg->trans), cfg->spd, cfg->spd_len);
    }
    if (cfg->infoframes & VIDEO_DIP_ENABLE_VS_HSW) {
        write_infoframe(i915, cfg->trans, VIDEO_DIP_ENABLE_VS_HSW,
                        HSW_TVIDEO_DIP_VS_DATA(cfg->trans), cfg->vs, cfg->vs_len);
    }
}
