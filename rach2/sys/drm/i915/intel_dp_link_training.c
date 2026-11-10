/*
 * intel_dp_link_training.c: a DisplayPort link's training at 8b/10b, after
 * Linux's display/intel_dp_link_training.c with its drm_dp_helper.c
 * helpers: the link's configuration in the sink's DPCD, clock recovery on
 * pattern 1, channel equalization on pattern 2 or 3, the fallback after
 * failures, and the end of training. LTTPRs train in their transparent
 * mode, since Linux detects them only from display version 10, whose AUX
 * timeout is long enough.
 */

#include "i915.h"
#include "i915_reg.h"
#include "intel_dp.h"

#include "../../klog.h"
#include "../../lib/kerrno.h"
#include "../../lib/kstring.h"

/* Linux's bounds: clock recovery tries 10 times on DPCD 1.4 and 80 before
 * it, and the same swing 5 times; equalization tries 5 times. */
#define CR_TRIES_14     10
#define CR_TRIES_OLD    80
#define VOLTAGE_TRIES   5
#define EQ_TRIES        5
#define LINK_STATUS_LEN 6

/** Whether lanes 0 to `lanes`-1 of `status` all have the bits `bits`,
 * Linux's drm_dp_clock_recovery_ok and drm_dp_channel_eq_ok's core. */
static int lanes_have(const uint8_t *status, uint32_t lanes, uint8_t bits)
{
    uint32_t i;

    for (i = 0; i < lanes; i++) {
        if (((status[i / 2] >> ((i & 1) * 4)) & bits) != bits) {
            return 0;
        }
    }
    return 1;
}

/** Clock recovery is done on every lane. */
static int cr_ok(const uint8_t *status, uint32_t lanes)
{
    return lanes_have(status, lanes, DP_LANE_CR_DONE);
}

/** Channel equalization is done: the lanes aligned, and every lane
 * recovered, equalized and locked. */
static int eq_ok(const uint8_t *status, uint32_t lanes)
{
    if (!(status[DP_LANE_ALIGN_STATUS_UPDATED - DP_LANE0_1_STATUS] & DP_INTERLANE_ALIGN_DONE)) {
        return 0;
    }
    return lanes_have(status, lanes,
                      DP_LANE_CR_DONE | DP_LANE_CHANNEL_EQ_DONE | DP_LANE_SYMBOL_LOCKED);
}

/** The swing lane `i` asks for, as DP_TRAINING_LANEx_SET places it. */
static uint8_t adjust_voltage(const uint8_t *status, uint32_t i)
{
    uint8_t v = status[DP_ADJUST_REQUEST_LANE0_1 - DP_LANE0_1_STATUS + i / 2];

    return (uint8_t)((v >> ((i & 1) * 4)) & 3u);
}

/** The pre-emphasis lane `i` asks for, as DP_TRAINING_LANEx_SET places
 * it. */
static uint8_t adjust_preemph(const uint8_t *status, uint32_t i)
{
    uint8_t v = status[DP_ADJUST_REQUEST_LANE0_1 - DP_LANE0_1_STATUS + i / 2];

    return (uint8_t)(((v >> ((i & 1) * 4)) & 0xcu) << 1);
}

/** The most swing a pre-emphasis allows, Linux's dp_voltage_max. */
static uint8_t voltage_max_for(uint8_t preemph)
{
    switch (preemph & DP_TRAIN_PRE_EMPHASIS_MASK) {
    case DP_TRAIN_PRE_EMPH_LEVEL_0:
        return DP_TRAIN_VOLTAGE_SWING_LEVEL_3;
    case DP_TRAIN_PRE_EMPH_LEVEL_1:
        return DP_TRAIN_VOLTAGE_SWING_LEVEL_2;
    case DP_TRAIN_PRE_EMPH_LEVEL_2:
        return DP_TRAIN_VOLTAGE_SWING_LEVEL_1;
    default:
        return DP_TRAIN_VOLTAGE_SWING_LEVEL_0;
    }
}

/**
 * Take the levels the sink asks for into `train_set`, Linux's
 * intel_dp_get_adjust_train for a source without per-lane levels: the
 * highest swing and pre-emphasis any lane asks, each held to what the
 * other allows and to the port's most, with the reached bits set there.
 */
static void get_adjust_train(struct intel_dp *dp, const uint8_t *status)
{
    uint8_t v = 0, p = 0, vmax = intel_ddi_dp_voltage_max();
    uint32_t i;

    for (i = 0; i < dp->lane_count; i++) {
        if (adjust_voltage(status, i) > v) {
            v = adjust_voltage(status, i);
        }
        if (adjust_preemph(status, i) > p) {
            p = adjust_preemph(status, i);
        }
    }
    if (p >= DP_TRAIN_PRE_EMPH_LEVEL_3) {
        p = DP_TRAIN_PRE_EMPH_LEVEL_3 | DP_TRAIN_MAX_PRE_EMPHASIS_REACHED;
    }
    if (v > voltage_max_for(p)) {
        v = voltage_max_for(p);
    }
    if (v >= vmax) {
        v = vmax | DP_TRAIN_MAX_SWING_REACHED;
    }
    for (i = 0; i < 4; i++) {
        dp->train_set[i] = (uint8_t)(v | p);
    }
}

/** Whether every lane's swing and pre-emphasis sum to 3 with the swing's
 * reached bit set, Linux's intel_dp_link_max_vswing_reached. */
static int max_vswing_reached(const struct intel_dp *dp)
{
    uint32_t i;

    for (i = 0; i < dp->lane_count; i++) {
        uint8_t t = dp->train_set[i];

        if (!(t & DP_TRAIN_MAX_SWING_REACHED) ||
            (t & DP_TRAIN_VOLTAGE_SWING_MASK) +
                    ((t & DP_TRAIN_PRE_EMPHASIS_MASK) >> DP_TRAIN_PRE_EMPHASIS_SHIFT) !=
                3) {
            return 0;
        }
    }
    return 1;
}

/** Whether a lane's adjust request differs between `a` and `b`. */
static int adjust_changed(const struct intel_dp *dp, const uint8_t *a, const uint8_t *b)
{
    uint32_t i;

    for (i = 0; i < dp->lane_count; i++) {
        if ((adjust_voltage(a, i) | adjust_preemph(a, i)) !=
            (adjust_voltage(b, i) | adjust_preemph(b, i))) {
            return 1;
        }
    }
    return 0;
}

/** Wait `us` microseconds, sleeping for a millisecond or more. */
static void train_delay(struct intel_dp *dp, uint32_t us)
{
    if (us >= 1000u) {
        intel_display_msleep(dp->i915, us / 1000u);
    } else {
        i915_delay_us(&dp->i915->hw, us);
    }
}

/** The delay of each read of the status: clock recovery's 100 us, or for
 * DPCD before 1.4 the interval the sink names, and equalization's 400 us
 * or the interval. Linux's drm_dp_read_clock_recovery_delay and
 * drm_dp_read_channel_eq_delay. */
static uint32_t status_delay(const struct intel_dp *dp, int cr)
{
    uint32_t interval = dp->dpcd[DP_TRAINING_AUX_RD_INTERVAL] & DP_TRAINING_AUX_RD_MASK;

    if (cr && dp->dpcd[DP_DPCD_REV] >= DP_DPCD_REV_14) {
        return 100u;
    }
    if (interval == 0) {
        return cr ? 100u : 400u;
    }
    return interval * 4000u;
}

/** Read the link's six status bytes; 0, or -EIO. */
static int32_t read_status(struct intel_dp *dp, uint8_t *status)
{
    return intel_dp_dpcd_read(dp, DP_LANE0_1_STATUS, status, LINK_STATUS_LEN) == LINK_STATUS_LEN
               ? 0
               : -EIO;
}

/** Drive pattern `pat` on the port and write it, with the lanes' levels,
 * to the sink, Linux's intel_dp_set_link_train; 0, or -EIO. */
static int32_t set_link_train(struct intel_dp *dp, uint8_t pat)
{
    uint8_t buf[5];

    intel_ddi_set_link_train(dp, pat);
    buf[0] = pat;
    k_memcpy(buf + 1, dp->train_set, dp->lane_count);
    return intel_dp_dpcd_write(dp, DP_TRAINING_PATTERN_SET, buf, dp->lane_count + 1u) ==
                   (int32_t)(dp->lane_count + 1u)
               ? 0
               : -EIO;
}

/** Drive the levels `train_set` names and write them to the sink, Linux's
 * intel_dp_update_link_train; 0, or -EIO. */
static int32_t update_link_train(struct intel_dp *dp)
{
    intel_ddi_set_signal_levels(dp);
    return intel_dp_dpcd_write(dp, DP_TRAINING_LANE0_SET, dp->train_set, dp->lane_count) ==
                   (int32_t)dp->lane_count
               ? 0
               : -EIO;
}

/** Whether the sink takes enhanced framing, Linux's
 * drm_dp_enhanced_frame_cap. */
static int enhanced_framing(const struct intel_dp *dp)
{
    return dp->dpcd[DP_DPCD_REV] >= DP_DPCD_REV_11 &&
           (dp->dpcd[DP_MAX_LANE_COUNT] & DP_ENHANCED_FRAME_CAP);
}

/**
 * Put the port on pattern 1 and write the link's configuration to the
 * sink, Linux's intel_dp_prepare_link_train: 8b/10b without spread, then
 * the rate as DP_LINK_BW_SET's code, or eDP 1.4's index into its rate
 * table after a reread of the table, and the lanes with enhanced
 * framing.
 */
static void prepare_link_train(struct intel_dp *dp)
{
    uint8_t cfg[2], lanes = (uint8_t)dp->lane_count, rates[2 * DP_MAX_SUPPORTED_RATES];
    int i, select = 0;

    intel_ddi_prepare_link_retrain(dp, enhanced_framing(dp));
    if (enhanced_framing(dp)) {
        lanes |= DP_LANE_COUNT_ENHANCED_FRAME_EN;
    }
    for (i = 0; dp->use_rate_select && i < dp->nsink_rates; i++) {
        if (dp->sink_rates[i] == dp->link_rate) {
            select = i;
        }
    }
    if (dp->use_rate_select) {
        (void)intel_dp_dpcd_read(dp, DP_SUPPORTED_LINK_RATES, rates, sizeof(rates));
    }
    cfg[0] = 0;
    cfg[1] = DP_SET_ANSI_8B10B;
    (void)intel_dp_dpcd_write(dp, DP_DOWNSPREAD_CTRL, cfg, 2);
    if (!dp->use_rate_select) {
        cfg[0] = (uint8_t)(dp->link_rate / 27000u);
        cfg[1] = lanes;
        (void)intel_dp_dpcd_write(dp, DP_LINK_BW_SET, cfg, 2);
    } else {
        (void)intel_dp_dpcd_writeb(dp, DP_LANE_COUNT_SET, lanes);
        (void)intel_dp_dpcd_writeb(dp, DP_LINK_RATE_SET, (uint8_t)select);
    }
}

/**
 * Clock recovery on pattern 1 with scrambling off, Linux's
 * intel_dp_link_training_clock_recovery: from the lowest levels, each
 * status read after the sink's delay, the levels it asks for taken until
 * every lane has its clock, the same request 5 times, or the swing at its
 * most. 0, or -EIO.
 */
static int32_t clock_recovery(struct intel_dp *dp)
{
    uint8_t status[LINK_STATUS_LEN], old[LINK_STATUS_LEN];
    uint32_t delay = status_delay(dp, 1);
    int tries, max = dp->dpcd[DP_DPCD_REV] >= DP_DPCD_REV_14 ? CR_TRIES_14 : CR_TRIES_OLD;
    int voltage_tries = 1, max_vswing = 0;

    k_memset(dp->train_set, 0, sizeof(dp->train_set));
    k_memset(old, 0, sizeof(old));
    intel_ddi_set_signal_levels(dp);
    if (set_link_train(dp, DP_TRAINING_PATTERN_1 | DP_LINK_SCRAMBLING_DISABLE) != 0) {
        klog(KLOG_ERR, "i915", "%s: failed to enable link training", dp->name);
        return -EIO;
    }
    for (tries = 0; tries < max; tries++) {
        train_delay(dp, delay);
        if (read_status(dp, status) != 0) {
            klog(KLOG_ERR, "i915", "%s: failed to get link status", dp->name);
            return -EIO;
        }
        if (cr_ok(status, dp->lane_count)) {
            return 0;
        }
        if (voltage_tries == VOLTAGE_TRIES || max_vswing) {
            klog(KLOG_INFO, "i915", "%s: clock recovery: %s", dp->name,
                 max_vswing ? "the most swing reached" : "the same swing tried 5 times");
            return -EIO;
        }
        get_adjust_train(dp, status);
        if (update_link_train(dp) != 0) {
            return -EIO;
        }
        voltage_tries = adjust_changed(dp, old, status) ? 1 : voltage_tries + 1;
        k_memcpy(old, status, sizeof(old));
        max_vswing = max_vswing_reached(dp);
    }
    klog(KLOG_ERR, "i915", "%s: clock recovery failed %d times, giving up", dp->name, max);
    return -EIO;
}

/**
 * Channel equalization, Linux's intel_dp_link_training_channel_equalization:
 * pattern 3 when the sink takes it (Haswell sends it), else 2, both with
 * scrambling off, up to 5 status reads while every lane keeps its clock.
 * 0, or -EIO.
 */
static int32_t channel_equalization(struct intel_dp *dp)
{
    uint8_t status[LINK_STATUS_LEN];
    uint8_t pat =
        dp->dpcd[DP_DPCD_REV] >= DP_DPCD_REV_12 && (dp->dpcd[DP_MAX_LANE_COUNT] & DP_TPS3_SUPPORTED)
            ? DP_TRAINING_PATTERN_3
            : DP_TRAINING_PATTERN_2;
    uint32_t delay = status_delay(dp, 0);
    int tries;

    if (set_link_train(dp, (uint8_t)(pat | DP_LINK_SCRAMBLING_DISABLE)) != 0) {
        klog(KLOG_ERR, "i915", "%s: failed to start channel equalization", dp->name);
        return -EIO;
    }
    for (tries = 0; tries < EQ_TRIES; tries++) {
        train_delay(dp, delay);
        if (read_status(dp, status) != 0) {
            klog(KLOG_ERR, "i915", "%s: failed to get link status", dp->name);
            return -EIO;
        }
        if (!cr_ok(status, dp->lane_count)) {
            klog(KLOG_INFO, "i915", "%s: clock recovery lost in channel equalization", dp->name);
            return -EIO;
        }
        if (eq_ok(status, dp->lane_count)) {
            return 0;
        }
        get_adjust_train(dp, status);
        if (update_link_train(dp) != 0) {
            return -EIO;
        }
    }
    klog(KLOG_INFO, "i915", "%s: channel equalization failed 5 times", dp->name);
    return -EIO;
}

/** One training at the link's rate and lanes, the port left on its idle
 * pattern; 0, or -EIO. Linux's intel_dp_link_train_all_phys. */
static int32_t train_once(struct intel_dp *dp)
{
    int32_t ret;

    prepare_link_train(dp);
    ret = clock_recovery(dp);
    if (ret == 0) {
        ret = channel_equalization(dp);
    }
    intel_ddi_set_idle_link_train(dp);
    return ret;
}

/**
 * Choose the link to train next after two failures, Linux's
 * intel_dp_get_link_train_fallback_values: for eDP first the sink's most;
 * then the next lower rate at the same lanes or, at the lowest, half the
 * lanes at the highest rate, for eDP only while the panel's mode at 18
 * bits a pixel still fits. 0, or -EIO when no lower link is left.
 */
static int32_t fallback(struct intel_dp *dp, uint32_t mode_clock)
{
    uint32_t rate = 0, lanes = dp->lane_count;
    int i;

    if (dp->edp && !dp->use_max_params) {
        dp->use_max_params = 1;
        klog(KLOG_INFO, "i915", "%s: retrying link training with the most parameters", dp->name);
        return 0;
    }
    for (i = 0; i < dp->nrates; i++) {
        if (dp->rates[i] < dp->link_rate && dp->rates[i] > rate) {
            rate = dp->rates[i];
        }
    }
    if (rate == 0) {
        if (lanes == 1) {
            return -EIO;
        }
        lanes >>= 1;
        rate = dp->rates[dp->nrates - 1];
    }
    if (dp->edp && (mode_clock * 18u + 7u) / 8u > rate * lanes) {
        klog(KLOG_INFO, "i915", "%s: retrying link training with the same parameters", dp->name);
        return 0;
    }
    klog(KLOG_INFO, "i915", "%s: reducing the link from %ux%u to %ux%u", dp->name,
         (unsigned)dp->lane_count, (unsigned)dp->link_rate, (unsigned)lanes, (unsigned)rate);
    dp->link_max_rate = rate;
    dp->link_max_lanes = lanes;
    return 0;
}

int32_t intel_dp_start_link_train(struct intel_dp *dp, uint32_t mode_clock)
{
    int32_t ret = train_once(dp);

    if (ret == 0) {
        dp->train_failures = 0;
        return 0;
    }
    /* Linux checks the link at once after a first failure and trains it
     * again; a second failure in a row falls back. */
    if (++dp->train_failures < 2) {
        ret = train_once(dp);
        if (ret == 0) {
            dp->train_failures = 0;
            return 0;
        }
        dp->train_failures++;
    }
    if (!dp->edp && !intel_dp_connected(dp)) {
        klog(KLOG_INFO, "i915", "%s: link training failed on a disconnected sink", dp->name);
        return -EIO;
    }
    klog(KLOG_ERR, "i915", "%s: link training failed at %u kHz on %u lanes", dp->name,
         (unsigned)dp->link_rate, (unsigned)dp->lane_count);
    if (fallback(dp, mode_clock) != 0) {
        klog(KLOG_ERR, "i915", "%s: can't reduce link training parameters after failure", dp->name);
        return -EIO;
    }
    return -EAGAIN;
}

int intel_dp_link_ok(struct intel_dp *dp)
{
    uint8_t status[LINK_STATUS_LEN];

    if (read_status(dp, status) != 0) {
        return -EIO;
    }
    if (eq_ok(status, dp->lane_count)) {
        return 1;
    }
    klog(KLOG_INFO, "i915", "%s: link not ok, status %02x %02x %02x", dp->name, (unsigned)status[0],
         (unsigned)status[1], (unsigned)status[2]);
    return 0;
}

void intel_dp_stop_link_train(struct intel_dp *dp)
{
    uint8_t off = DP_TRAINING_PATTERN_DISABLE;

    dp->link_trained = 1;
    (void)intel_dp_dpcd_write(dp, DP_TRAINING_PATTERN_SET, &off, 1);
    intel_ddi_set_link_train(dp, DP_TRAINING_PATTERN_DISABLE);
}
