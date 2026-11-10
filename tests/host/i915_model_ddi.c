/*
 * i915_model_ddi.c: DDI B to D of the host model (i915_model_ddi.h): the
 * GMBUS controller and GPIO pins from 0xc5000 with a DDC behind each, the
 * AUX channels of B to D from 0xe4100 with their DisplayPort sinks, the
 * ports' buffers and transports from 0x64100 to 0x643ff, and the PCH's
 * hotplug detection register.
 */

#include "i915_model_ddi.h"
#include "i915_model_ring.h"

#include "drm/i915/i915.h"
#include "drm/i915/i915_reg.h"
#include "drm/i915/intel_dp.h"

static const struct i915_model_block i915_model_ddi_gmbus;

/* The I2C target's place in a transfer. */
enum { I2C_IDLE, I2C_ADDR, I2C_ADDR_ACK, I2C_WRITE, I2C_WRITE_ACK, I2C_READ, I2C_READ_ACK };

/* The DP++ adaptor's I2C address. */
#define DDC_ADAPTOR_ADDR 0x40

/* The GMBUS pin of each GPIO register, by its index (Linux's gmbus_pins on
 * a Lynx Point PCH, read the other way). */
static const uint32_t gpio_pin[6] = {GMBUS_PIN_VGADDC, GMBUS_PIN_SSC, GMBUS_PIN_PANEL,
                                     GMBUS_PIN_DPC,    GMBUS_PIN_DPB, GMBUS_PIN_DPD};

/* The PCH's interrupt of each hotplug pin. */
static const uint32_t ddi_sde_bit[3] = {SDE_PORTB_HOTPLUG_CPT, SDE_PORTC_HOTPLUG_CPT,
                                        SDE_PORTD_HOTPLUG_CPT};

struct i915_model_ddi *i915_model_ddi(struct i915_model *m)
{
    int i;

    for (i = 0; i < i915_model_nblocks(); i++) {
        if (i915_model_block_at(i) == &i915_model_ddi_gmbus) {
            return (struct i915_model_ddi *)m->priv[i];
        }
    }
    return 0;
}

/* ---- the DDC targets ---- */

/** The DDC of GMBUS pin `pin`, or NULL. */
static struct i915_model_ddc *ddc_of(struct i915_model_ddi *d, uint32_t pin)
{
    return pin < MODEL_DDC_PINS ? &d->ddc[pin] : 0;
}

/** Whether a target of `c` answers at `addr`. */
static int ddc_answers(const struct i915_model_ddc *c, uint32_t addr)
{
    return c && ((c->present && (addr == DDC_ADDR || addr == DDC_SEGMENT_ADDR)) ||
                 (c->adaptor && addr == DDC_ADAPTOR_ADDR));
}

/** The next byte the target at `addr` sends. */
static uint8_t ddc_read(struct i915_model_ddc *c, uint32_t addr)
{
    uint32_t at;

    if (addr == DDC_ADAPTOR_ADDR) {
        return c->adaptor_regs[c->adaptor_offset++ & 0xffu];
    }
    at = c->segment * 256u + c->offset;
    c->offset = (c->offset + 1u) & 0xffu;
    return at < c->edid_len ? c->edid[at] : 0xffu;
}

/** A byte written to the target at `addr`, `first` of its transfer: the
 * segment pointer, the EDID's offset, or the adaptor's offset and
 * registers. */
static void ddc_write(struct i915_model_ddc *c, uint32_t addr, int first, uint8_t b)
{
    if (addr == DDC_SEGMENT_ADDR) {
        c->segment = b;
    } else if (addr == DDC_ADDR && first) {
        c->offset = b;
    } else if (addr == DDC_ADAPTOR_ADDR && first) {
        c->adaptor_offset = b;
    } else if (addr == DDC_ADAPTOR_ADDR) {
        c->adaptor_regs[c->adaptor_offset++ & 0xffu] = b;
    }
}

/* ---- GMBUS ---- */

/** A write transfer's next bytes, at most four of `word`. */
static void gmbus_take(struct i915_model_ddi *d, uint32_t word, int first)
{
    struct i915_model_ddc *c = ddc_of(d, d->pin);
    int i;

    for (i = 0; i < 4 && d->left > 0; i++) {
        ddc_write(c, d->addr, first && i == 0, (uint8_t)(word >> (8 * i)));
        d->left--;
    }
    d->gmbus2 |= GMBUS_HW_RDY | (d->left == 0 ? GMBUS_HW_WAIT_PHASE : 0);
}

/**
 * GMBUS1 with SW_RDY: a STOP ends the transfers and resets the segment
 * pointer; a cycle starts a transfer, which a target that does not answer
 * NAKs, a hung controller never answers, a read fills the FIFO and a
 * write takes the bytes GMBUS3 holds.
 */
static void gmbus_command(struct i915_model *m, struct i915_model_ddi *d, uint32_t value)
{
    struct i915_model_ddc *c = ddc_of(d, d->pin);

    if (value & GMBUS_CYCLE_STOP) {
        if (c) {
            c->segment = 0;
        }
        d->gmbus2 = 0;
        d->started = 0;
        return;
    }
    if (d->gmbus_hangs > 0) {
        d->gmbus_hangs--;
        d->gmbus2 = GMBUS_ACTIVE;
        d->started = 0;
        return;
    }
    d->addr = (value >> GMBUS_SLAVE_ADDR_SHIFT) & 0x7fu;
    d->read = (value & GMBUS_SLAVE_READ) != 0;
    d->left = (value >> GMBUS_BYTE_COUNT_SHIFT) & 0x1ffu;
    if (!ddc_answers(c, d->addr)) {
        d->gmbus2 = GMBUS_SATOER;
        d->started = 0;
        return;
    }
    c->gmbus_xfers++;
    d->started = 1;
    d->gmbus2 = GMBUS_ACTIVE;
    if (value & GMBUS_CYCLE_INDEX) {
        ddc_write(c, d->addr, 1, (uint8_t)(value >> GMBUS_SLAVE_INDEX_SHIFT));
    }
    if (d->read) {
        d->gmbus2 |= d->left ? GMBUS_HW_RDY : GMBUS_HW_WAIT_PHASE;
    } else {
        gmbus_take(d, i915_model_peek(m, GMBUS3), !(value & GMBUS_CYCLE_INDEX));
    }
}

/** A read of GMBUS2, or of GMBUS3, which gives a read's next four bytes. */
static uint32_t gmbus_read_reg(struct i915_model *m, struct i915_model_ddi *d, uint32_t reg)
{
    struct i915_model_ddc *c = ddc_of(d, d->pin);
    uint32_t v = 0;
    int i;

    if (reg == GMBUS2) {
        return d->gmbus2;
    }
    if (reg != GMBUS3 || !d->started || !d->read) {
        return i915_model_peek(m, reg);
    }
    for (i = 0; i < 4 && d->left > 0; i++) {
        v |= (uint32_t)ddc_read(c, d->addr) << (8 * i);
        c->gmbus_bytes++;
        d->left--;
    }
    if (d->left == 0) {
        d->gmbus2 = (d->gmbus2 & ~GMBUS_HW_RDY) | GMBUS_HW_WAIT_PHASE;
    }
    return v;
}

/* ---- bit-banging ---- */

/** The clock line's level: low while the master drives it low. */
static int scl_level(const struct i915_model_i2c *b)
{
    return !(b->scl_out && !b->scl_val);
}

/** The data line's level: low while the master or the target drives it. */
static int sda_level(const struct i915_model_i2c *b)
{
    return !(b->sda_out && !b->sda_val) && !b->target_low;
}

/** The target loads byte `v` to send and drives its first bit. */
static void i2c_load(struct i915_model_i2c *b, uint32_t v)
{
    b->shift = v;
    b->bits = 0;
    b->target_low = !(v & 0x80u);
    b->state = I2C_READ;
}

/** The clock's rise: the target samples the data line. */
static void i2c_rise(struct i915_model_i2c *b)
{
    int sda = sda_level(b);

    if (b->state == I2C_ADDR || b->state == I2C_WRITE) {
        b->shift = (b->shift << 1) | (uint32_t)sda;
        b->bits++;
    } else if (b->state == I2C_READ_ACK) {
        b->acked = !sda;
    }
}

/** The clock's fall: the target acknowledges, sends its next bit, or lets
 * the line go. */
static void i2c_fall(struct i915_model_i2c *b, struct i915_model_ddc *c)
{
    switch (b->state) {
    case I2C_ADDR:
        if (b->bits == 8) {
            b->addr = (b->shift >> 1) & 0x7fu;
            b->read = b->shift & 1u;
            b->state = ddc_answers(c, b->addr) ? I2C_ADDR_ACK : I2C_IDLE;
            b->target_low = b->state == I2C_ADDR_ACK;
            if (b->target_low) {
                c->bit_xfers++;
            }
        }
        break;
    case I2C_ADDR_ACK:
        b->target_low = 0;
        b->first = 1;
        b->bits = 0;
        b->shift = 0;
        if (b->read) {
            i2c_load(b, ddc_read(c, b->addr));
            c->bit_bytes++;
        } else {
            b->state = I2C_WRITE;
        }
        break;
    case I2C_WRITE:
        if (b->bits == 8) {
            ddc_write(c, b->addr, b->first, (uint8_t)b->shift);
            b->first = 0;
            b->target_low = 1;
            b->state = I2C_WRITE_ACK;
        }
        break;
    case I2C_WRITE_ACK:
        b->target_low = 0;
        b->bits = 0;
        b->shift = 0;
        b->state = I2C_WRITE;
        break;
    case I2C_READ:
        b->bits++;
        if (b->bits < 8) {
            b->target_low = !((b->shift >> (7u - b->bits)) & 1u);
        } else {
            b->target_low = 0;
            b->state = I2C_READ_ACK;
        }
        break;
    case I2C_READ_ACK:
        if (b->acked) {
            i2c_load(b, ddc_read(c, b->addr));
            c->bit_bytes++;
        } else {
            b->state = I2C_IDLE;
        }
        break;
    default:
        break;
    }
}

/**
 * A write of GPIO register `idx`: the masks say which of the pins'
 * directions and levels it sets; then the target follows the lines'
 * changes, a START or STOP while the clock is high, or a clock edge.
 */
static void gpio_write(struct i915_model *m, struct i915_model_ddi *d, uint32_t reg, uint32_t value)
{
    uint32_t idx = (reg - GPIO(0)) / 4u;
    struct i915_model_i2c *b = &d->gpio[idx];
    struct i915_model_ddc *c = ddc_of(d, gpio_pin[idx]);
    int scl = scl_level(b), sda = sda_level(b);

    i915_model_poke(m, reg, value & (GPIO_CLOCK_PULLUP_DIS | GPIO_DATA_PULLUP_DIS));
    if (value & GPIO_CLOCK_DIR_MASK) {
        b->scl_out = (value & GPIO_CLOCK_DIR_OUT) != 0;
    }
    if (value & GPIO_CLOCK_VAL_MASK) {
        b->scl_val = (value & GPIO_CLOCK_VAL_OUT) != 0;
    }
    if (value & GPIO_DATA_DIR_MASK) {
        b->sda_out = (value & GPIO_DATA_DIR_OUT) != 0;
    }
    if (value & GPIO_DATA_VAL_MASK) {
        b->sda_val = (value & GPIO_DATA_VAL_OUT) != 0;
    }
    if (scl && scl_level(b) && sda != sda_level(b)) {
        b->target_low = 0;
        b->bits = 0;
        b->shift = 0;
        b->state = sda_level(b) ? I2C_IDLE : I2C_ADDR;
        if (sda_level(b) && c) {
            c->segment = 0;
        }
    } else if (!scl && scl_level(b)) {
        i2c_rise(b);
    } else if (scl && !scl_level(b)) {
        i2c_fall(b, c);
    }
}

/** A GPIO register's read: the pull-ups as written and the lines' levels. */
static uint32_t gpio_read(struct i915_model *m, struct i915_model_ddi *d, uint32_t reg)
{
    const struct i915_model_i2c *b = &d->gpio[(reg - GPIO(0)) / 4u];

    return i915_model_peek(m, reg) | (scl_level(b) ? GPIO_CLOCK_VAL_IN : 0) |
           (sda_level(b) ? GPIO_DATA_VAL_IN : 0);
}

/** A read of a GPIO pin, of GMBUS2 or of GMBUS3; the rest as written. */
static uint32_t gmbus_block_read(struct i915_model *m, void *priv, uint32_t reg)
{
    struct i915_model_ddi *d = (struct i915_model_ddi *)priv;

    if (reg >= GPIO(0) && reg <= GPIO(5)) {
        return gpio_read(m, d, reg);
    }
    return gmbus_read_reg(m, d, reg);
}

/** A write of GMBUS or a GPIO pin. */
static void gmbus_block_write(struct i915_model *m, void *priv, uint32_t reg, uint32_t value)
{
    struct i915_model_ddi *d = (struct i915_model_ddi *)priv;

    if (reg >= GPIO(0) && reg <= GPIO(5)) {
        gpio_write(m, d, reg, value);
        return;
    }
    i915_model_poke(m, reg, value);
    if (reg == GMBUS0) {
        d->pin = value & 7u;
        if (value == 0) {
            d->gmbus2 = 0;
            d->started = 0;
        }
    } else if (reg == GMBUS1 && (value & GMBUS_SW_CLR_INT)) {
        d->gmbus2 &= ~(GMBUS_SATOER | GMBUS_HW_RDY | GMBUS_HW_WAIT_PHASE);
        d->started = 0;
    } else if (reg == GMBUS1 && (value & GMBUS_SW_RDY)) {
        gmbus_command(m, d, value);
    } else if (reg == GMBUS3 && d->started && !d->read && d->left > 0) {
        gmbus_take(d, value, 0);
    }
}

static const struct i915_model_block i915_model_ddi_gmbus = {
    "gmbus",          GPIO(0),           GMBUS5, sizeof(struct i915_model_ddi), 0,
    gmbus_block_read, gmbus_block_write, 0};

/* ---- the AUX channels and their sinks ---- */

/** The port whose AUX register `reg` is, 1 to 3. */
static int aux_port(uint32_t reg)
{
    return (int)((reg - DP_AUX_CH_CTL(1)) / 0x100u) + 1;
}

/** The lanes' status a read of DP_LANE0_1_STATUS finds while the port's
 * buffer and transport are on: clock recovery under pattern 1, all of it
 * under pattern 2 or 3 or on a trained link, and the link trained when
 * pattern 2 or 3 finds every lane done. */
static void ddi_sink_status(struct i915_model *m, int port, struct i915_model_dp_sink *s)
{
    uint32_t pattern = s->dpcd[DP_TRAINING_PATTERN_SET] & DP_TRAINING_PATTERN_MASK;
    uint32_t lanes = s->dpcd[DP_LANE_COUNT_SET] & 0x1fu, lane;
    uint8_t st = 0;

    if (!(i915_model_peek(m, DP_TP_CTL(port)) & DP_TP_CTL_ENABLE) ||
        !(i915_model_peek(m, DDI_BUF_CTL(port)) & DDI_BUF_CTL_ENABLE)) {
        lanes = 0;
        pattern = 0;
    } else if (pattern == 1) {
        st = DP_LANE_CR_DONE;
    } else if (pattern >= 2 || s->trained) {
        st = DP_LANE_CR_DONE | DP_LANE_CHANNEL_EQ_DONE | DP_LANE_SYMBOL_LOCKED;
    }
    s->dpcd[DP_LANE0_1_STATUS] = 0;
    s->dpcd[DP_LANE0_1_STATUS + 1] = 0;
    for (lane = 0; lane < lanes && lane < 4; lane++) {
        s->dpcd[DP_LANE0_1_STATUS + lane / 2] |= (uint8_t)(st << (4 * (lane & 1u)));
    }
    s->dpcd[DP_LANE0_1_STATUS + 2] = st & DP_LANE_SYMBOL_LOCKED ? DP_INTERLANE_ALIGN_DONE : 0;
    s->dpcd[DP_LANE0_1_STATUS + 4] = 0;
    s->dpcd[DP_LANE0_1_STATUS + 5] = 0;
    if (pattern >= 2 && lanes > 0) {
        s->trained = 1;
        s->trained_bw = s->dpcd[DP_LINK_BW_SET];
        s->trained_lanes = lanes;
    }
}

/** A native write of `n` bytes at `addr`: a new link setting or training
 * pattern ends the link's training. */
static void sink_native_write(struct i915_model_dp_sink *s, uint32_t addr, const uint8_t *in,
                              uint32_t n)
{
    uint32_t i;

    for (i = 0; i < n; i++) {
        if (addr + i < sizeof(s->dpcd)) {
            s->dpcd[addr + i] = in[i];
        }
        if (addr + i == DP_LINK_BW_SET || addr + i == DP_LANE_COUNT_SET ||
            (addr + i == DP_TRAINING_PATTERN_SET && (in[i] & DP_TRAINING_PATTERN_MASK))) {
            s->trained = 0;
            s->normal = 0;
        }
    }
}

/** An I2C-over-AUX message to `addr` of request `req` and `len` bytes. */
static uint32_t sink_i2c(struct i915_model_dp_sink *s, uint32_t req, uint32_t addr,
                         const uint8_t *tx, uint8_t *rx, uint32_t len)
{
    uint32_t i, at;

    if (addr != DDC_ADDR && addr != DDC_SEGMENT_ADDR) {
        rx[0] = DP_AUX_I2C_REPLY_NACK << 4;
        return 1;
    }
    if ((req & ~DP_AUX_I2C_MOT) == DP_AUX_I2C_READ) {
        for (i = 0; i < len; i++) {
            at = s->segment * 256u + s->offset++;
            rx[1 + i] = at < s->edid_len ? s->edid[at] : 0;
        }
        return 1 + len;
    }
    if (len > 0 && addr == DDC_SEGMENT_ADDR) {
        s->segment = tx[4];
    } else if (len > 0) {
        s->offset = tx[4];
    }
    if (!(req & DP_AUX_I2C_MOT) && addr == DDC_SEGMENT_ADDR) {
        s->segment = 0;
    }
    return 1;
}

/**
 * Answer the message the control `ctl` of `port` sends: a sink that is not
 * there times out; else the native reply and its bytes, or the I2C
 * reply, into the data registers, and the control's value once done into
 * the sink's `reply_ctl`.
 */
static void ddi_sink_message(struct i915_model *m, int port, struct i915_model_dp_sink *s,
                             uint32_t ctl)
{
    uint8_t tx[20], rx[20];
    uint32_t size = (ctl & DP_AUX_CH_CTL_MESSAGE_SIZE_MASK) >> 20, i, addr, len, n = 1, req;
    uint32_t done =
        (ctl & ~(DP_AUX_CH_CTL_SEND_BUSY | DP_AUX_CH_CTL_MESSAGE_SIZE_MASK | DP_AUX_CH_CTL_DONE |
                 DP_AUX_CH_CTL_TIME_OUT_ERROR | DP_AUX_CH_CTL_RECEIVE_ERROR)) |
        DP_AUX_CH_CTL_DONE;

    s->messages++;
    if (!s->present) {
        s->reply_ctl = done | DP_AUX_CH_CTL_TIME_OUT_ERROR;
        s->busy_us = 400u;
        return;
    }
    s->busy_us = 80u;
    for (i = 0; i < 20; i++) {
        tx[i] = (uint8_t)(i915_model_peek(m, DP_AUX_CH_DATA(port, i / 4)) >> ((3 - i % 4) * 8));
        rx[i] = 0;
    }
    req = tx[0] >> 4;
    addr = (uint32_t)(tx[0] & 0xf) << 16 | (uint32_t)tx[1] << 8 | tx[2];
    len = size > 3 ? tx[3] + 1u : 0;
    if (req == DP_AUX_NATIVE_READ) {
        if (addr <= DP_LANE0_1_STATUS + 5 && addr + len > DP_LANE0_1_STATUS) {
            ddi_sink_status(m, port, s);
        }
        for (i = 0; i < len; i++) {
            rx[1 + i] = addr + i < sizeof(s->dpcd) ? s->dpcd[addr + i] : 0;
        }
        n = 1 + len;
    } else if (req == DP_AUX_NATIVE_WRITE) {
        sink_native_write(s, addr, tx + 4, len);
    } else {
        n = sink_i2c(s, req, addr, tx, rx, len);
    }
    for (i = 0; i < n; i += 4) {
        i915_model_poke(m, DP_AUX_CH_DATA(port, i / 4),
                        (uint32_t)rx[i] << 24 | (uint32_t)rx[i + 1] << 16 |
                            (uint32_t)rx[i + 2] << 8 | rx[i + 3]);
    }
    s->reply_ctl = done | DP_AUX_CH_CTL_MESSAGE_SIZE(n);
}

/** A write of an AUX register: a control with SEND_BUSY sends, another
 * clears the bits of DONE and the errors written as 1. */
static void aux_write(struct i915_model *m, void *priv, uint32_t reg, uint32_t value)
{
    struct i915_model_ddi *d = i915_model_ddi(m);
    const uint32_t w1c =
        DP_AUX_CH_CTL_DONE | DP_AUX_CH_CTL_TIME_OUT_ERROR | DP_AUX_CH_CTL_RECEIVE_ERROR;
    int port = aux_port(reg);
    uint32_t old = i915_model_peek(m, reg);

    (void)priv;
    if (reg != DP_AUX_CH_CTL(port)) {
        i915_model_poke(m, reg, value);
    } else if ((value & DP_AUX_CH_CTL_SEND_BUSY) && !(old & DP_AUX_CH_CTL_SEND_BUSY)) {
        ddi_sink_message(m, port, &d->sink[port - 1], value);
        i915_model_poke(m, reg, (value & ~w1c) | DP_AUX_CH_CTL_SEND_BUSY);
    } else {
        i915_model_poke(m, reg,
                        (value & ~(w1c | DP_AUX_CH_CTL_SEND_BUSY)) | (old & w1c & ~value) |
                            (old & DP_AUX_CH_CTL_SEND_BUSY));
    }
}

/** Time passes: each message in flight is answered once its time ends. */
static void aux_tick(struct i915_model *m, void *priv, uint32_t us)
{
    struct i915_model_ddi *d = i915_model_ddi(m);
    int port;

    (void)priv;
    for (port = 1; port <= 3; port++) {
        struct i915_model_dp_sink *s = &d->sink[port - 1];

        if (!(i915_model_peek(m, DP_AUX_CH_CTL(port)) & DP_AUX_CH_CTL_SEND_BUSY)) {
            continue;
        }
        if (us < s->busy_us) {
            s->busy_us -= us;
            continue;
        }
        s->busy_us = 0;
        i915_model_poke(m, DP_AUX_CH_CTL(port), s->reply_ctl);
    }
}

static const struct i915_model_block i915_model_ddi_aux = {
    "pch aux", DP_AUX_CH_CTL(1), DP_AUX_CH_DATA(3, 4), 1, 0, 0, aux_write, aux_tick};

/* ---- the ports' buffers and transports ---- */

/** The port of a DDI register of B to D. */
static int ddi_port(uint32_t reg)
{
    return (int)((reg - 0x64000u) / 0x100u);
}

/** The buffer's idle bit follows its enable, and the transport's idle done
 * its idle pattern. */
static uint32_t ports_read(struct i915_model *m, void *priv, uint32_t reg)
{
    int port = ddi_port(reg);
    uint32_t v = i915_model_peek(m, reg), tp;

    (void)priv;
    if (reg == DDI_BUF_CTL(port)) {
        v = (v & ~DDI_BUF_IS_IDLE) | ((v & DDI_BUF_CTL_ENABLE) ? 0 : DDI_BUF_IS_IDLE);
    } else if (reg == DP_TP_STATUS(port)) {
        tp = i915_model_peek(m, DP_TP_CTL(port));
        v = (tp & DP_TP_CTL_ENABLE) && (tp & DP_TP_CTL_LINK_TRAIN_MASK) == DP_TP_CTL_LINK_TRAIN_IDLE
                ? DP_TP_STATUS_IDLE_DONE
                : 0;
    }
    return v;
}

/** The buffer or the transport going off loses the sink's link; the
 * normal pattern on a trained link sends pictures. */
static void ports_write(struct i915_model *m, void *priv, uint32_t reg, uint32_t value)
{
    struct i915_model_dp_sink *s = &i915_model_ddi(m)->sink[ddi_port(reg) - 1];
    int port = ddi_port(reg);

    (void)priv;
    i915_model_poke(m, reg, value);
    if ((reg == DDI_BUF_CTL(port) && !(value & DDI_BUF_CTL_ENABLE)) ||
        (reg == DP_TP_CTL(port) && !(value & DP_TP_CTL_ENABLE))) {
        s->trained = 0;
        s->normal = 0;
    }
    if (reg == DP_TP_CTL(port) && (value & DP_TP_CTL_ENABLE) &&
        (value & DP_TP_CTL_LINK_TRAIN_MASK) == DP_TP_CTL_LINK_TRAIN_NORMAL && s->trained) {
        s->normal = 1;
    }
}

static const struct i915_model_block i915_model_ddi_ports = {
    "ddi ports", 0x64100u, 0x643fcu, 1, 0, ports_read, ports_write, 0};

/* ---- hotplug ---- */

/* PCH_PORT_HOTPLUG's status bits, written 1 to clear: ports B to D and A. */
#define MODEL_HOTPLUG_STATUS                                                                   \
    (PORT_HOTPLUG_STATUS_MASK(0) | PORT_HOTPLUG_STATUS_MASK(1) | PORT_HOTPLUG_STATUS_MASK(2) | \
     PORT_HOTPLUG_STATUS_MASK(3))

/** PCH_PORT_HOTPLUG: the enables as written, the status bits cleared by a
 * 1. */
static void hotplug_write(struct i915_model *m, void *priv, uint32_t reg, uint32_t value)
{
    uint32_t old = i915_model_peek(m, reg);

    (void)priv;
    i915_model_poke(m, reg,
                    (value & ~MODEL_HOTPLUG_STATUS) | (old & MODEL_HOTPLUG_STATUS & ~value));
}

static const struct i915_model_block i915_model_ddi_hotplug = {
    "pch hotplug", PCH_PORT_HOTPLUG, PCH_PORT_HOTPLUG, 1, 0, 0, hotplug_write, 0};

void i915_model_ddi_pulse(struct i915_model *m, int pin, int connected, int long_pulse)
{
    uint32_t isr = i915_model_peek(m, SDEISR), hp = i915_model_peek(m, PCH_PORT_HOTPLUG);

    i915_model_ddi(m)->pulses++;
    i915_model_poke(m, SDEISR, connected ? isr | ddi_sde_bit[pin] : isr & ~ddi_sde_bit[pin]);
    if (!(hp & PORT_HOTPLUG_ENABLE(pin))) {
        return;
    }
    hp |= long_pulse ? PORT_HOTPLUG_LONG_DETECT(pin) : PORT_HOTPLUG_SHORT_DETECT(pin);
    i915_model_poke(m, PCH_PORT_HOTPLUG, hp);
    i915_model_irq_event(m, I915_IRQ_SDE, ddi_sde_bit[pin]);
}
