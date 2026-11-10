/*
 * i915_model_sink.c: the eDP panel of the host model (i915_model_sink.h):
 * DDI A's registers from 0x64000 to 0x640ff, its AUX channel with the
 * sink's DPCD, EDID and link training behind it, and the panel power
 * sequencer's registers from 0xc7200 to 0xc7210.
 */

#include "i915_model_sink.h"

#include "drm/i915/i915.h"
#include "drm/i915/i915_reg.h"
#include "drm/i915/intel_dp.h"

static const struct i915_model_block i915_model_sink_block;
static const struct i915_model_block i915_model_panel_block;

const unsigned char lenovo_panel_edid[128] = {
    0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x06, 0xaf, 0xec, 0x40, 0x00, 0x00, 0x00, 0x00,
    0x29, 0x17, 0x01, 0x04, 0x95, 0x22, 0x13, 0x78, 0x02, 0xbb, 0xf5, 0x94, 0x55, 0x54, 0x90, 0x27,
    0x23, 0x50, 0x54, 0x00, 0x00, 0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0xce, 0x1d, 0x56, 0xc0, 0x50, 0x00, 0x30, 0x30, 0x08, 0x0a,
    0x31, 0x00, 0x58, 0xc1, 0x10, 0x00, 0x00, 0x18, 0x00, 0x00, 0x00, 0x0f, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00, 0xfe, 0x00, 0x41,
    0x55, 0x4f, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x00, 0x00, 0x00, 0xfe,
    0x00, 0x42, 0x31, 0x35, 0x36, 0x58, 0x54, 0x4e, 0x30, 0x34, 0x2e, 0x30, 0x20, 0x0a, 0x00, 0xee};

/* The Lenovo's memory latencies as its pcode leaves them in MCH_SSKPD
 * (Linux's i915_pri_wm_latency: 20, 4, 36, 90 and 160), and the PCH's raw
 * clock, 24 MHz on Lynx Point LP. */
#define LENOVO_SSKPD_LO (90u << 20 | 36u << 12 | 4u << 4)
#define LENOVO_SSKPD_HI (20u << 24 | 160u)
#define LENOVO_RAWCLK   24u

/* Linux's index_to_dp_signal_levels: the swing and pre-emphasis of each
 * DDI_BUF_TRANS_SELECT. */
static const uint8_t sink_levels[10] = {0x00, 0x08, 0x10, 0x18, 0x01, 0x09, 0x11, 0x02, 0x0a, 0x03};

/** The state of the block `b` in `m`. */
static void *block_state(struct i915_model *m, const struct i915_model_block *b)
{
    int i;

    for (i = 0; i < i915_model_nblocks(); i++) {
        if (i915_model_block_at(i) == b) {
            return m->priv[i];
        }
    }
    return 0;
}

struct i915_model_sink *i915_model_sink(struct i915_model *m)
{
    return (struct i915_model_sink *)block_state(m, &i915_model_sink_block);
}

struct i915_model_panel *i915_model_panel(struct i915_model *m)
{
    return (struct i915_model_panel *)block_state(m, &i915_model_panel_block);
}

/** The DPCD byte at `addr`, 0 past the model's 2 KiB. */
static uint8_t dpcd_get(const struct i915_model_sink *s, uint32_t addr)
{
    return addr < sizeof(s->dpcd) ? s->dpcd[addr] : 0;
}

/** The firmware's panel: lit, its link trained at 2.7 Gb/s on one lane. */
static void sink_reset(struct i915_model *m, void *priv)
{
    struct i915_model_sink *s = (struct i915_model_sink *)priv;
    int i;

    s->dpcd[DP_DPCD_REV] = LENOVO_SINK_DPCD_REV;
    s->dpcd[DP_MAX_LINK_RATE] = LENOVO_SINK_MAX_RATE;
    s->dpcd[DP_MAX_LANE_COUNT] = LENOVO_SINK_MAX_LANES;
    s->dpcd[DP_MAX_DOWNSPREAD] = DP_MAX_DOWNSPREAD_0_5;
    s->dpcd[DP_MAIN_LINK_CHANNEL_CODING] = DP_CAP_ANSI_8B10B;
    s->dpcd[DP_LINK_BW_SET] = DP_LINK_BW_2_7;
    s->dpcd[DP_LANE_COUNT_SET] = 1 | DP_LANE_COUNT_ENHANCED_FRAME_EN;
    s->dpcd[DP_SET_POWER] = DP_SET_POWER_D0;
    for (i = 0; i < 128; i++) {
        s->edid[i] = lenovo_panel_edid[i];
    }
    s->edid_len = 128;
    s->cr_swing = 1;
    s->eq_preemph = 1;
    s->trained = 1;
    s->normal = 1;
    s->trained_bw = DP_LINK_BW_2_7;
    s->trained_lanes = 1;
    i915_model_poke(m, DP_TP_CTL(0),
                    DP_TP_CTL_ENABLE | DP_TP_CTL_ENHANCED_FRAME_ENABLE |
                        DP_TP_CTL_LINK_TRAIN_NORMAL);
    i915_model_poke(m, PCH_RAWCLK_FREQ, LENOVO_RAWCLK);
    i915_model_poke(m, MCH_SSKPD, LENOVO_SSKPD_LO);
    i915_model_poke(m, MCH_SSKPD_HI, LENOVO_SSKPD_HI);
}

/** The link's rate code that PORT_CLK_SEL's LCPLL gives, or 0. */
static uint32_t clock_bw(struct i915_model *m)
{
    switch (i915_model_peek(m, PORT_CLK_SEL(0)) & PORT_CLK_SEL_MASK) {
    case PORT_CLK_SEL_LCPLL_810:
        return DP_LINK_BW_1_62;
    case PORT_CLK_SEL_LCPLL_1350:
        return DP_LINK_BW_2_7;
    case PORT_CLK_SEL_LCPLL_2700:
        return DP_LINK_BW_5_4;
    default:
        return 0;
    }
}

/** Whether the source drives the link as DPCD's link settings and
 * training pattern say: the buffer and transport on, the clock of the
 * rate, the port's width of the lanes, and the same pattern. */
static int link_up(struct i915_model *m, const struct i915_model_sink *s, uint32_t lanes,
                   uint32_t pattern)
{
    uint32_t tp = i915_model_peek(m, DP_TP_CTL(0)), buf = i915_model_peek(m, DDI_BUF_CTL(0));
    uint32_t src = tp & DP_TP_CTL_LINK_TRAIN_MASK;

    if (!(tp & DP_TP_CTL_ENABLE) || !(buf & DDI_BUF_CTL_ENABLE) || lanes < 1 || lanes > 4 ||
        clock_bw(m) != s->dpcd[DP_LINK_BW_SET] ||
        ((buf & DDI_PORT_WIDTH_MASK) >> 1) + 1u != lanes) {
        return 0;
    }
    return (pattern == DP_TRAINING_PATTERN_1 && src == DP_TP_CTL_LINK_TRAIN_PAT1) ||
           (pattern == DP_TRAINING_PATTERN_2 && src == DP_TP_CTL_LINK_TRAIN_PAT2);
}

/** The status nibble of lane `i`: clock recovery done at a swing of
 * `cr_swing` or more, and equalization at a pre-emphasis of `eq_preemph`
 * or more, while the link is `up` and the buffer drives the levels the
 * lane's DPCD set names; `*levels` is cleared when it does not. */
static uint8_t lane_status(struct i915_model *m, const struct i915_model_sink *s, uint32_t i,
                           uint32_t pattern, int up, int *levels)
{
    uint32_t sel = (i915_model_peek(m, DDI_BUF_CTL(0)) & DDI_BUF_EMP_MASK) >> 24;
    uint8_t set = s->dpcd[DP_TRAINING_LANE0_SET + i];
    uint32_t swing = set & DP_TRAIN_VOLTAGE_SWING_MASK;
    uint32_t pre = (set & DP_TRAIN_PRE_EMPHASIS_MASK) >> DP_TRAIN_PRE_EMPHASIS_SHIFT;
    uint8_t st = 0;

    if (sel >= sizeof(sink_levels) ||
        sink_levels[sel] != (set & (DP_TRAIN_VOLTAGE_SWING_MASK | DP_TRAIN_PRE_EMPHASIS_MASK))) {
        *levels = 0;
    }
    if (up && *levels && swing >= s->cr_swing) {
        st |= DP_LANE_CR_DONE;
        if (pattern == DP_TRAINING_PATTERN_2 && pre >= s->eq_preemph) {
            st |= DP_LANE_CHANNEL_EQ_DONE | DP_LANE_SYMBOL_LOCKED;
        }
    }
    return st;
}

/**
 * Fill the link status, DPCD 0x202 to 0x207, from what the source sends
 * (`link_up`, `lane_status`); the adjust requests ask for the swing and,
 * in equalization, the pre-emphasis the sink needs. A status read that
 * finds every lane equalized in pattern 2 ends the training.
 */
static void link_status(struct i915_model *m, struct i915_model_sink *s)
{
    uint32_t lanes = s->dpcd[DP_LANE_COUNT_SET] & DP_MAX_LANE_COUNT_MASK;
    uint32_t pattern = s->dpcd[DP_TRAINING_PATTERN_SET] & DP_TRAINING_PATTERN_MASK, i;
    int up = link_up(m, s, lanes, pattern), levels = 1, cr = 1, eq = 1;
    uint8_t want =
        (uint8_t)(s->cr_swing | (pattern == DP_TRAINING_PATTERN_2 ? s->eq_preemph << 2 : 0));

    for (i = 0; i < 6; i++) {
        s->dpcd[DP_LANE0_1_STATUS + i] = 0;
    }
    for (i = 0; i < lanes && i < 4; i++) {
        uint8_t st = lane_status(m, s, i, pattern, up, &levels);

        cr = cr && (st & DP_LANE_CR_DONE);
        eq = eq && (st & DP_LANE_CHANNEL_EQ_DONE);
        s->dpcd[DP_LANE0_1_STATUS + i / 2] |= (uint8_t)(st << ((i & 1) * 4));
        s->dpcd[DP_ADJUST_REQUEST_LANE0_1 + i / 2] |= (uint8_t)(want << ((i & 1) * 4));
    }
    s->levels_bad += up && !levels;
    if (cr && eq && lanes >= 1) {
        s->dpcd[DP_LANE_ALIGN_STATUS_UPDATED] = DP_INTERLANE_ALIGN_DONE;
    }
    s->cr_reads += up && pattern == DP_TRAINING_PATTERN_1;
    s->eq_reads += up && pattern == DP_TRAINING_PATTERN_2;
    if (up && pattern == DP_TRAINING_PATTERN_2 && cr && eq) {
        s->trained = 1;
        s->trained_bw = s->dpcd[DP_LINK_BW_SET];
        s->trained_lanes = lanes;
    }
}

/** A native read of `n` bytes at `addr` into `out`. */
static void native_read(struct i915_model *m, struct i915_model_sink *s, uint32_t addr,
                        uint8_t *out, uint32_t n)
{
    uint32_t i;

    if (addr <= DP_LANE0_1_STATUS + 5 && addr + n > DP_LANE0_1_STATUS) {
        link_status(m, s);
    }
    for (i = 0; i < n; i++) {
        out[i] = dpcd_get(s, addr + i);
    }
    s->native_reads++;
}

/** A native write of `n` bytes of `in` at `addr`: a new training pattern
 * or link setting ends the link's training. */
static void native_write(struct i915_model_sink *s, uint32_t addr, const uint8_t *in, uint32_t n)
{
    uint32_t i;

    for (i = 0; i < n; i++) {
        if (addr + i < sizeof(s->dpcd)) {
            s->dpcd[addr + i] = in[i];
        }
        if (addr + i == DP_LINK_BW_SET || addr + i == DP_LANE_COUNT_SET ||
            (addr + i == DP_TRAINING_PATTERN_SET && (in[i] & DP_TRAINING_PATTERN_MASK) != 0)) {
            s->trained = 0;
            s->normal = 0;
        }
    }
    s->native_writes++;
}

/**
 * Answer the message in the data registers that `ctl` sends: its reply,
 * the native and I2C reply in the high nibble of the first byte and the
 * read's bytes after it, into the data registers, and the control's
 * value once done into `s->reply_ctl`.
 */
static void sink_message(struct i915_model *m, struct i915_model_sink *s, uint32_t ctl)
{
    uint8_t tx[20], rx[20];
    uint32_t size = (ctl & DP_AUX_CH_CTL_MESSAGE_SIZE_MASK) >> 20, i, addr, len, n = 1, req;
    uint32_t done =
        (ctl & ~(DP_AUX_CH_CTL_SEND_BUSY | DP_AUX_CH_CTL_MESSAGE_SIZE_MASK | DP_AUX_CH_CTL_DONE |
                 DP_AUX_CH_CTL_TIME_OUT_ERROR | DP_AUX_CH_CTL_RECEIVE_ERROR)) |
        DP_AUX_CH_CTL_DONE;

    s->messages++;
    if (!s->powered || s->timeouts > 0) {
        if (s->powered) {
            s->timeouts--;
        }
        s->reply_ctl = done | DP_AUX_CH_CTL_TIME_OUT_ERROR;
        s->busy_us = SINK_TIMEOUT_US;
        return;
    }
    s->busy_us = SINK_AUX_US;
    if (s->rx_errors > 0) {
        s->rx_errors--;
        s->reply_ctl = done | DP_AUX_CH_CTL_RECEIVE_ERROR;
        return;
    }
    for (i = 0; i < 20; i++) {
        tx[i] = (uint8_t)(i915_model_peek(m, DP_AUX_CH_DATA(0, i / 4)) >> ((3 - i % 4) * 8));
        rx[i] = 0;
    }
    req = tx[0] >> 4;
    addr = (uint32_t)(tx[0] & 0xf) << 16 | (uint32_t)tx[1] << 8 | tx[2];
    len = size > 3 ? tx[3] + 1u : 0;
    if (s->zero_replies > 0) {
        s->zero_replies--;
        s->reply_ctl = done;
        return;
    }
    if (s->defers > 0) {
        s->defers--;
        rx[0] = DP_AUX_NATIVE_REPLY_DEFER << 4;
    } else if (req == DP_AUX_NATIVE_READ) {
        native_read(m, s, addr, rx + 1, len);
        n = 1 + len;
    } else if (req == DP_AUX_NATIVE_WRITE) {
        native_write(s, addr, tx + 4, len);
    } else if (s->i2c_defers > 0) {
        s->i2c_defers--;
        rx[0] = DP_AUX_I2C_REPLY_DEFER << 4;
    } else if (addr != DDC_ADDR && addr != DDC_SEGMENT_ADDR) {
        rx[0] = DP_AUX_I2C_REPLY_NACK << 4;
    } else if ((req & ~DP_AUX_I2C_MOT) == DP_AUX_I2C_READ) {
        for (i = 0; i < len; i++) {
            uint32_t at = s->segment * 256u + s->offset++;

            rx[1 + i] = at < s->edid_len ? s->edid[at] : 0;
        }
        n = 1 + len;
        if (len > 0) {
            s->i2c_reads++;
        }
    } else if (len > 0) {
        if (addr == DDC_SEGMENT_ADDR) {
            s->segment = tx[4];
        } else {
            s->offset = tx[4];
        }
    }
    for (i = 0; i < n; i += 4) {
        i915_model_poke(m, DP_AUX_CH_DATA(0, i / 4),
                        (uint32_t)rx[i] << 24 | (uint32_t)rx[i + 1] << 16 |
                            (uint32_t)rx[i + 2] << 8 | rx[i + 3]);
    }
    s->reply_ctl = done | DP_AUX_CH_CTL_MESSAGE_SIZE(n);
}

/** The buffer's idle bit follows its enable, and the transport's idle done
 * its idle pattern. */
static uint32_t sink_read(struct i915_model *m, void *priv, uint32_t reg)
{
    uint32_t v = i915_model_peek(m, reg), tp;

    (void)priv;
    if (reg == DDI_BUF_CTL(0)) {
        v = (v & ~DDI_BUF_IS_IDLE) | ((v & DDI_BUF_CTL_ENABLE) ? 0 : DDI_BUF_IS_IDLE);
    } else if (reg == DP_TP_STATUS(0)) {
        tp = i915_model_peek(m, DP_TP_CTL(0));
        v = (tp & DP_TP_CTL_ENABLE) && (tp & DP_TP_CTL_LINK_TRAIN_MASK) == DP_TP_CTL_LINK_TRAIN_IDLE
                ? DP_TP_STATUS_IDLE_DONE
                : 0;
    }
    return v;
}

/**
 * A write: a control with SEND_BUSY sends its message, which is busy
 * until the sink's time passes; another clears the bits of DONE and the
 * errors it writes as 1. The buffer or the transport going off loses the
 * link; the transport going to the normal pattern on a trained link
 * sends pictures.
 */
static void sink_write(struct i915_model *m, void *priv, uint32_t reg, uint32_t value)
{
    struct i915_model_sink *s = (struct i915_model_sink *)priv;
    const uint32_t w1c =
        DP_AUX_CH_CTL_DONE | DP_AUX_CH_CTL_TIME_OUT_ERROR | DP_AUX_CH_CTL_RECEIVE_ERROR;
    uint32_t old = i915_model_peek(m, reg);

    if (reg == DP_AUX_CH_CTL(0)) {
        if ((value & DP_AUX_CH_CTL_SEND_BUSY) && !(old & DP_AUX_CH_CTL_SEND_BUSY)) {
            sink_message(m, s, value);
            i915_model_poke(m, reg, (value & ~w1c) | DP_AUX_CH_CTL_SEND_BUSY);
        } else {
            i915_model_poke(m, reg,
                            (value & ~(w1c | DP_AUX_CH_CTL_SEND_BUSY)) | (old & w1c & ~value) |
                                (old & DP_AUX_CH_CTL_SEND_BUSY));
        }
        return;
    }
    i915_model_poke(m, reg, value);
    if ((reg == DDI_BUF_CTL(0) && !(value & DDI_BUF_CTL_ENABLE)) ||
        (reg == DP_TP_CTL(0) && !(value & DP_TP_CTL_ENABLE))) {
        s->trained = 0;
        s->normal = 0;
    }
    if (reg == DP_TP_CTL(0) && (value & DP_TP_CTL_ENABLE) &&
        (value & DP_TP_CTL_LINK_TRAIN_MASK) == DP_TP_CTL_LINK_TRAIN_NORMAL && s->trained) {
        s->normal = 1;
    }
}

/** Time passes: a message in flight is answered. */
static void sink_tick(struct i915_model *m, void *priv, uint32_t us)
{
    struct i915_model_sink *s = (struct i915_model_sink *)priv;

    if (!(i915_model_peek(m, DP_AUX_CH_CTL(0)) & DP_AUX_CH_CTL_SEND_BUSY)) {
        return;
    }
    if (us < s->busy_us) {
        s->busy_us -= us;
        return;
    }
    s->busy_us = 0;
    i915_model_poke(m, DP_AUX_CH_CTL(0), s->reply_ctl);
}

static const struct i915_model_block i915_model_sink_block = {
    "edp sink", 0x64000u,  0x640fcu,   sizeof(struct i915_model_sink),
    sink_reset, sink_read, sink_write, sink_tick};

/* ---- The panel power sequencer ---- */

/** A delay field of PP_ON_DELAYS or PP_OFF_DELAYS in microseconds. */
static uint64_t delay_us(struct i915_model *m, uint32_t reg, int shift)
{
    return (uint64_t)((i915_model_peek(m, reg) >> shift) & PANEL_DELAY_FIELD_MASK) * 100u;
}

/** The power cycle delay PP_DIVISOR gives, in microseconds. */
static uint64_t cycle_us(struct i915_model *m)
{
    uint32_t f = i915_model_peek(m, PP_DIVISOR) & PANEL_POWER_CYCLE_DELAY_MASK;

    return f > 1 ? (uint64_t)(f - 1u) * 100000u : 0;
}

/** Whether the sink's logic has power: the panel's, or VDD forced on. */
static void panel_power_sink(struct i915_model *m, struct i915_model_panel *p)
{
    struct i915_model_sink *s = i915_model_sink(m);

    if (s) {
        s->powered = p->on || (i915_model_peek(m, PP_CONTROL) & EDP_FORCE_VDD);
    }
}

/** The firmware's panel, on, as the pipe block's reset left its
 * registers. */
static void panel_reset(struct i915_model *m, void *priv)
{
    struct i915_model_panel *p = (struct i915_model_panel *)priv;

    p->on = (i915_model_peek(m, PP_STATUS) & PP_ON) != 0;
    panel_power_sink(m, p);
}

/** PP_STATUS from the sequence under way. */
static uint32_t panel_read(struct i915_model *m, void *priv, uint32_t reg)
{
    struct i915_model_panel *p = (struct i915_model_panel *)priv;
    uint32_t v;

    if (reg != PP_STATUS) {
        return i915_model_peek(m, reg);
    }
    v = p->on ? PP_ON : 0;
    if (p->seq == 1) {
        v |= PP_SEQUENCE_POWER_UP | 0x9u;
    } else if (p->seq == 2) {
        v |= PP_SEQUENCE_POWER_DOWN | 0x1u;
    } else if (p->on) {
        v |= PP_SEQUENCE_STATE_ON_IDLE;
    }
    if (p->cycle) {
        v |= PP_CYCLE_DELAY_ACTIVE;
    }
    return v;
}

/** Begin a power up, now or once the power cycle ends. */
static void panel_up(struct i915_model *m, struct i915_model_panel *p)
{
    if (p->cycle) {
        p->up_pending = 1;
        return;
    }
    p->up_pending = 0;
    p->on = 1;
    p->seq = 1;
    p->left_us = delay_us(m, PP_ON_DELAYS, PANEL_POWER_UP_DELAY_SHIFT);
    panel_power_sink(m, p);
}

/** PP_CONTROL: the power on bit starts a sequence, and the backlight's
 * enable is timed. PP_STATUS takes no write. */
static void panel_write(struct i915_model *m, void *priv, uint32_t reg, uint32_t value)
{
    struct i915_model_panel *p = (struct i915_model_panel *)priv;
    uint32_t old = i915_model_peek(m, PP_CONTROL);

    if (reg == PP_STATUS) {
        return;
    }
    i915_model_poke(m, reg, value);
    if (reg != PP_CONTROL) {
        return;
    }
    if ((value & PANEL_POWER_ON) && !(old & PANEL_POWER_ON)) {
        panel_up(m, p);
    } else if (!(value & PANEL_POWER_ON) && (old & PANEL_POWER_ON)) {
        p->up_pending = 0;
        p->seq = 2;
        p->left_us = delay_us(m, PP_OFF_DELAYS, PANEL_POWER_DOWN_DELAY_SHIFT);
        p->down_from = m->now_us;
    }
    if ((value & EDP_BLC_ENABLE) && !(old & EDP_BLC_ENABLE)) {
        p->blc_on_at = m->now_us;
    } else if (!(value & EDP_BLC_ENABLE) && (old & EDP_BLC_ENABLE)) {
        p->blc_off_at = m->now_us;
    }
    panel_power_sink(m, p);
}

/** Time passes: the sequence under way and the power cycle run on. */
static void panel_tick(struct i915_model *m, void *priv, uint32_t us)
{
    struct i915_model_panel *p = (struct i915_model_panel *)priv;

    if (p->seq != 0) {
        if (p->left_us > us) {
            p->left_us -= us;
        } else if (p->seq == 1) {
            p->seq = 0;
            p->up_at = m->now_us;
            p->power_ups++;
        } else {
            p->seq = 0;
            p->on = 0;
            p->cycle = 1;
            p->cycle_us = cycle_us(m);
            p->down_at = m->now_us;
            p->power_downs++;
        }
    } else if (p->cycle) {
        if (p->cycle_us > us) {
            p->cycle_us -= us;
        } else {
            p->cycle = 0;
            if (p->up_pending) {
                panel_up(m, p);
            }
        }
    }
    panel_power_sink(m, p);
}

static const struct i915_model_block i915_model_panel_block = {
    "panel power", PP_STATUS,  PP_DIVISOR,  sizeof(struct i915_model_panel),
    panel_reset,   panel_read, panel_write, panel_tick};
