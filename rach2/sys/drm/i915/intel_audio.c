/*
 * intel_audio.c: HDMI and DisplayPort audio on Haswell, after Linux's
 * display/intel_audio.c: the ELD a pipe's codec reads, the transcoder's
 * audio enable and clocks, and the audio component that the HD Audio
 * driver of the HDMI codec binds to (sys/audio/acomp.h).
 *
 * Linux 6.12.115's lines, in display/intel_audio.c: the clocks,
 * audio_config_hdmi_pixel_clock (197), audio_config_hdmi_get_n (227) and
 * hsw_audio_config_update (389); the codec's enable and disable,
 * hsw_audio_codec_enable (542), hsw_audio_codec_disable (398),
 * intel_audio_codec_enable (726) and intel_audio_codec_disable (785);
 * intel_audio_compute_config (693); and the component,
 * i915_audio_component_get_power (981), _put_power (1013),
 * _get_cdclk_freq (1057), find_audio_state (1077), _sync_audio_rate (1116)
 * and _get_eld (1158).
 */

#include "i915.h"
#include "i915_reg.h"

#include "../../audio/acomp.h"
#include "../../klog.h"
#include "../../lib/kerrno.h"
#include "../../lib/kstring.h"

/* hdmi_audio_clock (intel_audio.c:87 of Linux 6.12.115): the pixel clocks
 * in kHz AUD_CONFIG names by its index, 25200 the default. */
static const uint32_t hdmi_audio_clock[] = {25175, 25200,  27000,  27027,  54000,  54054,  74176,
                                            74250, 148352, 148500, 296703, 297000, 593407, 594000};

#define HDMI_AUDIO_CLOCK_DEFAULT 1

/* An N for a sample rate at a TMDS clock in kHz, Linux's hdmi_aud_ncts
 * without the CTS, which the hardware computes. */
struct hdmi_aud_n {
    uint32_t rate, clock, n;
};

/* hdmi_aud_ncts_24bpp, _30bpp and _36bpp (intel_audio.c:113-187). */
static const struct hdmi_aud_n ncts_24bpp[] = {
    {32000, 296703, 5824},   {32000, 297000, 3072},   {32000, 593407, 5824},
    {32000, 594000, 3072},   {44100, 296703, 4459},   {44100, 297000, 4704},
    {44100, 593407, 8918},   {44100, 594000, 9408},   {88200, 296703, 8918},
    {88200, 297000, 9408},   {88200, 593407, 17836},  {88200, 594000, 18816},
    {176400, 296703, 17836}, {176400, 297000, 18816}, {176400, 593407, 35672},
    {176400, 594000, 37632}, {48000, 296703, 5824},   {48000, 297000, 5120},
    {48000, 593407, 5824},   {48000, 594000, 6144},   {96000, 296703, 11648},
    {96000, 297000, 10240},  {96000, 593407, 11648},  {96000, 594000, 12288},
    {192000, 296703, 23296}, {192000, 297000, 20480}, {192000, 593407, 23296},
    {192000, 594000, 24576}};

static const struct hdmi_aud_n ncts_30bpp[] = {
    {32000, 370878, 5824},   {32000, 371250, 6144},   {44100, 370878, 8918},
    {44100, 371250, 4704},   {88200, 370878, 17836},  {88200, 371250, 9408},
    {176400, 370878, 35672}, {176400, 371250, 18816}, {48000, 370878, 11648},
    {48000, 371250, 5120},   {96000, 370878, 23296},  {96000, 371250, 10240},
    {192000, 370878, 46592}, {192000, 371250, 20480}};

static const struct hdmi_aud_n ncts_36bpp[] = {
    {32000, 445054, 5824},   {32000, 445500, 4096},   {44100, 445054, 8918},
    {44100, 445500, 4704},   {88200, 445054, 17836},  {88200, 445500, 9408},
    {176400, 445054, 35672}, {176400, 445500, 18816}, {48000, 445054, 5824},
    {48000, 445500, 5120},   {96000, 445054, 11648},  {96000, 445500, 10240},
    {192000, 445054, 23296}, {192000, 445500, 20480}};

#define AUD_NELEM(a) (sizeof(a) / sizeof((a)[0]))

/** AUD_CONFIG's pixel clock field for `cfg`'s mode, Linux's
 * audio_config_hdmi_pixel_clock: the default for a clock not listed, and
 * for one above 148.5 MHz before display version 12. */
static uint32_t hdmi_pixel_clock(const struct intel_crtc_config *cfg)
{
    uint32_t i;

    for (i = 0; i < AUD_NELEM(hdmi_audio_clock); i++) {
        if (cfg->mode.clock == hdmi_audio_clock[i]) {
            break;
        }
    }
    if (cfg->mode.clock > 148500u || i == AUD_NELEM(hdmi_audio_clock)) {
        i = HDMI_AUDIO_CLOCK_DEFAULT;
    }
    return AUD_CONFIG_PIXEL_CLOCK_HDMI(i);
}

/** The N for `rate` at `cfg`'s TMDS clock and depth, Linux's
 * audio_config_hdmi_get_n; 0 to let the hardware choose. */
static uint32_t hdmi_get_n(const struct intel_crtc_config *cfg, int rate)
{
    const struct hdmi_aud_n *t = cfg->bpp == 36u   ? ncts_36bpp
                                 : cfg->bpp == 30u ? ncts_30bpp
                                                   : ncts_24bpp;
    uint32_t n = cfg->bpp == 36u   ? AUD_NELEM(ncts_36bpp)
                 : cfg->bpp == 30u ? AUD_NELEM(ncts_30bpp)
                                   : AUD_NELEM(ncts_24bpp);
    uint32_t i;

    for (i = 0; i < n; i++) {
        if ((uint32_t)rate == t[i].rate && cfg->port_clock == t[i].clock) {
            return t[i].n;
        }
    }
    return 0;
}

/** Program `cfg`'s transcoder's audio clocks, Linux's
 * hsw_audio_config_update: DisplayPort lets the hardware make Maud and
 * Naud; HDMI takes the pixel clock and, for a rate the tables have, N.
 * Under the audio mutex. */
static void config_update(struct drm_i915_private *i915, const struct intel_crtc_config *cfg)
{
    struct i915_hw *hw = &i915->hw;
    uint32_t tmp, n;
    int t = cfg->trans;

    if (cfg->output != INTEL_OUTPUT_HDMI) {
        i915_rmw(hw, HSW_AUD_CFG(t),
                 AUD_CONFIG_N_VALUE_INDEX | AUD_CONFIG_PIXEL_CLOCK_HDMI_MASK | AUD_CONFIG_N_MASK |
                     AUD_CONFIG_N_PROG_ENABLE,
                 AUD_CONFIG_N_VALUE_INDEX);
        return;
    }
    tmp = i915_read(hw, HSW_AUD_CFG(t));
    tmp &=
        ~(AUD_CONFIG_N_VALUE_INDEX | AUD_CONFIG_PIXEL_CLOCK_HDMI_MASK | AUD_CONFIG_N_PROG_ENABLE);
    tmp |= hdmi_pixel_clock(cfg);
    n = hdmi_get_n(cfg, i915->display.audio.sample_rate[cfg->port]);
    if (n != 0) {
        tmp &= ~AUD_CONFIG_N_MASK;
        tmp |= AUD_CONFIG_N(n) | AUD_CONFIG_N_PROG_ENABLE;
    }
    i915_write(hw, HSW_AUD_CFG(t), tmp);
    /* The hardware computes CTS. */
    i915_rmw(hw, HSW_AUD_M_CTS_ENABLE(t), AUD_M_CTS_M_PROG_ENABLE | AUD_M_CTS_M_VALUE_INDEX, 0);
}

int intel_audio_port_has_audio(int output, int port)
{
    return output == INTEL_OUTPUT_HDMI || (output == INTEL_OUTPUT_DP && port != INTEL_PORT_A);
}

void intel_audio_compute_config(const struct drm_connector_state *conn,
                                const struct drm_edid_hdmi_info *info,
                                struct intel_crtc_config *cfg)
{
    const struct drm_connector *connector = conn->connector;

    cfg->has_audio = 0;
    k_memset(cfg->eld, 0, sizeof(cfg->eld));
    if (!intel_audio_port_has_audio(cfg->output, cfg->port) ||
        (cfg->output == INTEL_OUTPUT_HDMI && !cfg->has_hdmi_sink)) {
        return;
    }
    if (conn->force_audio == DRM_FORCE_AUDIO_AUTO ? !info->has_audio
                                                  : conn->force_audio != DRM_FORCE_AUDIO_ON) {
        return;
    }
    if (!connector->eld[0]) {
        klog(KLOG_DEBUG, "i915", "audio: bogus ELD on connector %u", (unsigned)connector->base.id);
        return;
    }
    k_memcpy(cfg->eld, connector->eld, sizeof(cfg->eld));
    cfg->eld[DRM_ELD_AUD_SYNCH_DELAY] = (uint8_t)(drm_av_sync_delay(connector, &cfg->mode) / 2u);
    cfg->has_audio = 1;
}

void intel_audio_codec_enable(struct drm_i915_private *i915, int pipe,
                              const struct intel_crtc_config *cfg)
{
    struct intel_audio *a = &i915->display.audio;
    struct i915_hw *hw = &i915->hw;
    int t = cfg->trans;

    if (!cfg->has_audio) {
        return;
    }
    klog(KLOG_DEBUG, "i915", "audio: enable on pipe %c, port %c, %u bytes ELD", 'A' + pipe,
         'A' + cfg->port, (unsigned)drm_eld_size(cfg->eld));
    mutex_lock(&a->mutex);
    /* hsw_audio_codec_enable: presence detect, a vblank, the ELD marked
     * not valid, since the component carries it, then the clocks. */
    i915_rmw(hw, HSW_AUD_PIN_ELD_CP_VLD, 0, AUDIO_OUTPUT_ENABLE(t));
    intel_crtc_wait_for_next_vblank(i915, pipe);
    i915_rmw(hw, HSW_AUD_PIN_ELD_CP_VLD, AUDIO_ELD_VALID(t), 0);
    config_update(i915, cfg);
    a->state[t].port = cfg->port;
    a->state[t].output = cfg->output;
    k_memcpy(a->state[t].eld, cfg->eld, sizeof(a->state[t].eld));
    mutex_unlock(&a->mutex);
    /* Without DP MST the codec takes -1 for the device entry. */
    acomp_eld_notify(cfg->port, -1);
}

void intel_audio_codec_disable(struct drm_i915_private *i915, int pipe,
                               const struct intel_crtc_config *cfg)
{
    struct intel_audio *a = &i915->display.audio;
    struct i915_hw *hw = &i915->hw;
    int t = cfg->trans;

    if (!cfg->has_audio) {
        return;
    }
    klog(KLOG_DEBUG, "i915", "audio: disable on pipe %c, port %c", 'A' + pipe, 'A' + cfg->port);
    mutex_lock(&a->mutex);
    /* hsw_audio_codec_disable: the timestamps off, the ELD not valid, two
     * vblanks, then presence detect off. */
    i915_rmw(hw, HSW_AUD_CFG(t), AUD_CONFIG_N_VALUE_INDEX | AUD_CONFIG_N_MASK,
             AUD_CONFIG_N_PROG_ENABLE |
                 (cfg->output != INTEL_OUTPUT_HDMI ? AUD_CONFIG_N_VALUE_INDEX : 0u));
    i915_rmw(hw, HSW_AUD_PIN_ELD_CP_VLD, AUDIO_ELD_VALID(t), 0);
    intel_crtc_wait_for_next_vblank(i915, pipe);
    intel_crtc_wait_for_next_vblank(i915, pipe);
    i915_rmw(hw, HSW_AUD_PIN_ELD_CP_VLD, AUDIO_OUTPUT_ENABLE(t), 0);
    k_memset(&a->state[t], 0, sizeof(a->state[t]));
    mutex_unlock(&a->mutex);
    acomp_eld_notify(cfg->port, -1);
}

int intel_audio_hw_enabled(struct drm_i915_private *i915, int trans)
{
    if (trans < 0 || trans >= I915_NUM_PIPES) {
        return 0;
    }
    return (i915_read(&i915->hw, HSW_AUD_PIN_ELD_CP_VLD) & AUDIO_OUTPUT_ENABLE(trans)) != 0;
}

/* ------------------------------------------------------------------------
 * The audio component's operations, Linux's i915_audio_component_ops
 */

/** Linux's i915_audio_component_get_power: the device awake and the
 * display well, which holds POWER_DOMAIN_AUDIO_PLAYBACK on Haswell, on. */
static int32_t acomp_get_power(void *dev)
{
    struct drm_i915_private *i915 = (struct drm_i915_private *)dev;
    int32_t err;

    i915_rpm_get(i915);
    err = i915_power_get(i915);
    if (err != 0) {
        i915_rpm_put(i915);
        return err;
    }
    i915->display.audio.power_refcount++;
    return 0;
}

/** Linux's i915_audio_component_put_power. */
static void acomp_put_power(void *dev)
{
    struct drm_i915_private *i915 = (struct drm_i915_private *)dev;

    i915->display.audio.power_refcount--;
    i915_power_put(i915);
    i915_rpm_put(i915);
}

/** Linux's i915_audio_component_get_cdclk_freq: the CD clock in kHz. */
static int32_t acomp_get_cdclk_freq(void *dev)
{
    return (int32_t)((struct drm_i915_private *)dev)->display.cdclk;
}

/** The audio state of port `port`, device entry `pipe`, Linux's
 * find_audio_state with no DP MST: entry 0 or -1 finds the transcoder
 * whose output is on the port; NULL for none. Under the audio mutex. */
static struct intel_audio_state *find_audio_state(struct drm_i915_private *i915, int port, int pipe)
{
    int t;

    if (pipe > 0) {
        return NULL;
    }
    for (t = 0; t < I915_NUM_PIPES; t++) {
        if (i915->display.audio.state[t].port == port && port != 0) {
            return &i915->display.audio.state[t];
        }
    }
    return NULL;
}

/** Linux's i915_audio_component_sync_audio_rate: the port's rate kept
 * and its transcoder's clocks programmed for it. */
static int32_t acomp_sync_audio_rate(void *dev, int port, int pipe, int rate)
{
    struct drm_i915_private *i915 = (struct drm_i915_private *)dev;
    struct intel_audio *a = &i915->display.audio;
    struct intel_audio_state *st;
    int32_t err = acomp_get_power(dev);

    if (err != 0) {
        return err;
    }
    mutex_lock(&a->mutex);
    st = port > 0 && port < (int)AUD_NELEM(a->sample_rate) ? find_audio_state(i915, port, pipe)
                                                           : NULL;
    if (!st) {
        err = -ENODEV;
    } else {
        a->sample_rate[port] = rate;
        config_update(i915, &i915->display.crtc[st - a->state].cfg);
    }
    mutex_unlock(&a->mutex);
    acomp_put_power(dev);
    return err;
}

/** Linux's i915_audio_component_get_eld. */
static int32_t acomp_get_eld(void *dev, int port, int pipe, int *enabled, uint8_t *buf, int max)
{
    struct drm_i915_private *i915 = (struct drm_i915_private *)dev;
    struct intel_audio *a = &i915->display.audio;
    struct intel_audio_state *st;
    int32_t size;

    mutex_lock(&a->mutex);
    st = find_audio_state(i915, port, pipe);
    if (!st) {
        mutex_unlock(&a->mutex);
        *enabled = 0;
        return -EINVAL;
    }
    *enabled = 1;
    size = (int32_t)drm_eld_size(st->eld);
    k_memcpy(buf, st->eld, (uint32_t)(size < max ? size : max));
    mutex_unlock(&a->mutex);
    return size;
}

static const struct acomp_ops i915_acomp_ops = {
    acomp_get_power, acomp_put_power, acomp_get_cdclk_freq, acomp_sync_audio_rate, acomp_get_eld};

void intel_audio_init(struct drm_i915_private *i915)
{
    mutex_init(&i915->display.audio.mutex, "i915_audio");
}

void intel_audio_register(struct drm_i915_private *i915)
{
    if (!(i915->drm.features & DRIVER_MODESET) || i915->display.audio.registered) {
        return;
    }
    if (acomp_register_display(&i915_acomp_ops, i915) == 0) {
        i915->display.audio.registered = 1;
    }
}

void intel_audio_deinit(struct drm_i915_private *i915)
{
    if (i915->display.audio.registered) {
        acomp_unregister_display(i915);
        i915->display.audio.registered = 0;
    }
}
