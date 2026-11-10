/*
 * i915_model_sink.h: the eDP panel of the host model's HD 4400
 * (i915_model_sink.c): DDI A with its AUX channel and DisplayPort
 * transport, the panel's DPCD, EDID and link training behind them, and
 * the PCH's panel power sequencer, as the Lenovo's panel answers.
 */
#ifndef _I915_MODEL_SINK_H_
#define _I915_MODEL_SINK_H_

#include "i915_model.h"

/* The Lenovo panel's EDID (B156XTN04.0, tests/fixtures/lenovo-gpu's
 * modetest-connectors.out) and its DPCD as the model gives it: DPCD 1.1,
 * 2.7 Gb/s and one lane with enhanced framing, eDP's own registers 0. */
extern const unsigned char lenovo_panel_edid[128];
#define LENOVO_SINK_DPCD_REV  0x11
#define LENOVO_SINK_MAX_RATE  0x0a
#define LENOVO_SINK_MAX_LANES 0x81

/* DP_TP_CTL of DDI A as the firmware leaves it: the dump has no value, so
 * the model takes the one a trained SST link with enhanced framing has. */
#define LENOVO_FW_DP_TP_CTL_A 0x80040300u

/* How long the sink takes to answer an AUX message, and to time out. */
#define SINK_AUX_US     80u
#define SINK_TIMEOUT_US 400u

/*
 * The sink. `dpcd` is its DPCD from 0x000 to 0x7ff; the rest reads 0.
 * `edid` its EDID, `edid_len` bytes, with the I2C side's `offset` and
 * `segment`. A message in flight takes `busy_us` more microseconds.
 *
 * Faults a row asks for, each spent by one message: `timeouts` sends that
 * time out, `rx_errors` that meet a receive error, `defers` native DEFER
 * replies, `i2c_defers` I2C DEFER replies, `zero_replies` replies of no
 * bytes. `powered` says the panel's logic has power, VDD or the panel's.
 *
 * Link training: clock recovery needs a swing of `cr_swing` and channel
 * equalization a pre-emphasis of `eq_preemph`; the sink asks for them in
 * its adjust requests. `levels_bad` counts status reads while the
 * source's DDI_BUF_CTL drove other levels than DPCD's lane set said.
 * `trained` says training ended with the link up, at `trained_bw`
 * (DP_LINK_BW_SET) on `trained_lanes`; `cr_reads` and `eq_reads` count
 * the status reads of each phase. `normal` says the source's transport
 * sends the normal pattern on a trained link.
 *
 * The counts: `messages`, and the native reads and writes and the I2C
 * reads of the EDID.
 */
struct i915_model_sink {
    uint8_t dpcd[0x800];
    uint8_t edid[256];
    uint32_t edid_len;
    uint32_t offset, segment;
    uint32_t busy_us;
    uint32_t reply_ctl;
    uint32_t timeouts, rx_errors, defers, i2c_defers, zero_replies;
    int powered;
    uint32_t cr_swing, eq_preemph;
    uint32_t levels_bad;
    int trained;
    uint32_t trained_bw, trained_lanes;
    uint32_t cr_reads, eq_reads;
    int normal;
    uint32_t messages, native_reads, native_writes, i2c_reads;
};

/*
 * The panel power sequencer. `on` is PP_STATUS's PP_ON; `seq` the
 * sequence under way, 0, 1 power up or 2 power down, `left_us` of it to
 * run; `cycle` says the power cycle delay runs, `cycle_us` of it left.
 * `up_pending` says a power on waits for the cycle to end, as the
 * hardware holds it. The times on the model's clock of the last power up
 * finished, power down begun and finished, and the backlight's enable
 * set and cleared; `power_ups` and `power_downs` count them.
 */
struct i915_model_panel {
    int on;
    int seq;
    uint64_t left_us;
    int cycle;
    uint64_t cycle_us;
    int up_pending;
    uint64_t up_at, down_from, down_at, blc_on_at, blc_off_at;
    uint32_t power_ups, power_downs;
};

/** The sink of `m`. */
struct i915_model_sink *i915_model_sink(struct i915_model *m);

/** The panel power sequencer of `m`. */
struct i915_model_panel *i915_model_panel(struct i915_model *m);

#endif /* _I915_MODEL_SINK_H_ */
