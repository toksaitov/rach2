/*
 * intel_gmbus.c: the PCH's GMBUS I2C controller and its GPIO pins, after
 * Linux's display/intel_gmbus.c and i2c-algo-bit.c for Haswell, and the
 * read of a sink's EDID over any I2C bus, after drm_edid.c.
 */

#include "i915.h"
#include "i915_reg.h"
#include "intel_dp.h"

#include "../../klog.h"
#include "../../rtc.h"
#include "../../timer.h"
#include "../../lib/kerrno.h"
#include "../../lib/kstring.h"

/* Linux's bounds: four reads of an EDID block whose sum or header is bad,
 * five transfers of each read (drm_do_probe_ddc_edid). */
#define EDID_BLOCK_TRIES 4
#define DDC_TRIES        5

/* GMBUS's waits: 50 ms for a byte or a phase, 10 ms for the controller to
 * go idle (gmbus_wait, gmbus_wait_idle). Each spins GMBUS_SPIN_US, the
 * time of a cycle of four bytes at 100 kHz with room, and sleeps a
 * millisecond at a time after it. */
#define GMBUS_WAIT_US 50000u
#define GMBUS_IDLE_US 10000u
#define GMBUS_SPIN_US 500u

/* i2c-algo-bit's timing as Linux's GPIO buses set it: 10 us for a line to
 * rise or fall, I2C_RISEFALL_TIME, and 2.2 ms for a sink that stretches
 * the clock. */
#define BIT_UDELAY     10u
#define BIT_TIMEOUT_US 2200u
/* The tries of an address a sink did not acknowledge, the adapter's
 * retries plus one. */
#define BIT_RETRIES    1

/*
 * The pins of a Lynx Point PCH, Linux's gmbus_pins: the name and the GPIO
 * register index, by GMBUS pin.
 */
static const struct {
    const char *name;
    uint8_t gpio;
} gmbus_pins[GMBUS_NUM_PINS] = {
    {NULL, 0},        {"gmbus ssc", 1}, {"gmbus vga", 0}, {"gmbus panel", 2},
    {"gmbus dpc", 3}, {"gmbus dpb", 4}, {"gmbus dpd", 5},
};

/* ------------------------------------------------------------------------
 * The EDID
 */

/**
 * Read `len` bytes of EDID block `block` into `buf`, Linux's
 * drm_do_probe_ddc_edid: the segment pointer for blocks past the first
 * two, the offset, and the bytes, five times at most, and no more once the
 * bus says that no sink answers at the address. 0, or -EIO.
 */
static int32_t ddc_block(const struct intel_i2c_bus *bus, uint32_t block, uint8_t *buf,
                         uint32_t len)
{
    uint8_t start = (uint8_t)(block * 128u), segment = (uint8_t)(block >> 1);
    int xfers = segment ? 3 : 2, retries = DDC_TRIES;
    int32_t ret;

    do {
        struct intel_i2c_msg msgs[3];

        msgs[0].addr = DDC_SEGMENT_ADDR;
        msgs[0].read = 0;
        msgs[0].len = 1;
        msgs[0].buf = &segment;
        msgs[1].addr = DDC_ADDR;
        msgs[1].read = 0;
        msgs[1].len = 1;
        msgs[1].buf = &start;
        msgs[2].addr = DDC_ADDR;
        msgs[2].read = 1;
        msgs[2].len = len;
        msgs[2].buf = buf;
        ret = bus->xfer(bus->ctx, &msgs[3 - xfers], xfers);
        if (ret == -ENXIO) {
            break;
        }
    } while (ret != xfers && --retries);
    return ret == xfers ? 0 : -EIO;
}

int intel_i2c_probe_ddc(const struct intel_i2c_bus *bus)
{
    uint8_t out;

    return ddc_block(bus, 0, &out, 1) == 0;
}

/** Whether EDID block `b` sums to 0 and, for the base block, has the
 * EDID's header. */
static int edid_block_ok(const uint8_t *b, int base)
{
    static const uint8_t header[8] = {0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0};
    uint8_t sum = 0;
    int i;

    for (i = 0; i < 128; i++) {
        sum = (uint8_t)(sum + b[i]);
    }
    for (i = 0; base && i < (int)sizeof(header); i++) {
        if (b[i] != header[i]) {
            return 0;
        }
    }
    return sum == 0;
}

int32_t intel_edid_read(const struct intel_i2c_bus *bus, uint8_t *buf, uint32_t cap, uint32_t *len)
{
    uint32_t i, blocks;
    int try;

    *len = 0;
    if (cap < 128) {
        return -EINVAL;
    }
    for (try = 0; try < EDID_BLOCK_TRIES; try++) {
        if (ddc_block(bus, 0, buf, 128) != 0) {
            return -EIO;
        }
        if (edid_block_ok(buf, 1)) {
            break;
        }
    }
    if (try == EDID_BLOCK_TRIES) {
        return -EINVAL;
    }
    blocks = 1u + buf[126];
    if (blocks > cap / 128u) {
        klog(KLOG_WARN, "i915", "%s: EDID of %u blocks, %u kept", bus->name, (unsigned)blocks,
             (unsigned)(cap / 128u));
        blocks = cap / 128u;
    }
    for (i = 1; i < blocks; i++) {
        for (try = 0; try < EDID_BLOCK_TRIES; try++) {
            if (ddc_block(bus, i, buf + i * 128u, 128) != 0) {
                return -EIO;
            }
            if (edid_block_ok(buf + i * 128u, 0)) {
                break;
            }
        }
        if (try == EDID_BLOCK_TRIES) {
            /* Linux drops a bad extension; the ones before it stand. */
            klog(KLOG_WARN, "i915", "%s: EDID block %u bad, %u kept", bus->name, (unsigned)i,
                 (unsigned)i);
            break;
        }
    }
    *len = i * 128u;
    return 0;
}

/* ------------------------------------------------------------------------
 * GMBUS
 */

/** Reset the controller, Linux's intel_gmbus_reset: no pin, no interrupt. */
static void gmbus_reset(struct drm_i915_private *i915)
{
    i915_write(&i915->hw, GMBUS0, 0);
    i915_write(&i915->hw, GMBUS4, 0);
}

/**
 * Wait up to `bound_us` until `done` says so, as Linux's wait_for: GMBUS
 * waits sleep (intel_gmbus.c:394 in 6.12.115, wait_for's usleep_range at
 * i915_utils.h:237), so that a transfer that stalls for its whole bound
 * keeps no processor. The first GMBUS_SPIN_US are spun, since a sleep is
 * at least a tick here where Linux's first are 10 us; then `done` is read
 * after each sleep of a millisecond, the transfer kept whole meanwhile by
 * the controller's lock (gmbus_lock). The wait ends at a deadline on the
 * monotonic clock, as wait_for's `end__` (i915_utils.h:220), since a sleep
 * lasts two ticks or more; it ends sooner only when the time it counted,
 * which is never more than the time that passed, reaches the bound, as on
 * a clock that does not move while the scheduler is off. Whether the wait
 * has expired is read before `done`, so that a last read always follows
 * the deadline (i915_utils.h:225-236). 0, or -ETIMEDOUT, counted and
 * logged as `what`, as i915_wait_until does.
 */
static int32_t gmbus_poll(struct intel_gmbus *bus, const char *what,
                          int (*done)(struct i915_hw *, void *), void *arg, uint32_t bound_us)
{
    struct i915_hw *hw = &bus->i915->hw;
    uint64_t due =
        rtc_ticks_now() + drm_div64((uint64_t)bound_us * TICK_HZ + 999999u, 1000000u) + 1u;
    uint32_t waited = 0, step = 1;
    int expired;

    for (;;) {
        expired = waited >= bound_us || rtc_ticks_now() >= due;
        if (done(hw, arg)) {
            return 0;
        }
        if (expired) {
            hw->timeouts++;
            hw->last_timeout = what;
            klog(KLOG_ERR, "i915", "%s: timeout after %u us", what, (unsigned)bound_us);
            return -ETIMEDOUT;
        }
        if (waited < GMBUS_SPIN_US) {
            if (step > GMBUS_SPIN_US - waited) {
                step = GMBUS_SPIN_US - waited;
            }
            i915_delay_us(hw, step);
            waited += step;
            step *= 2u;
        } else {
            intel_display_msleep(bus->i915, 1);
            waited += 1000u;
        }
    }
}

/* What `gmbus_status_done` waits for: the GMBUS2 bit, and the last read. */
struct gmbus_wait_arg {
    uint32_t bit;
    uint32_t status;
};

/** Whether GMBUS2 shows the awaited bit or a NAK. */
static int gmbus_status_done(struct i915_hw *hw, void *arg)
{
    struct gmbus_wait_arg *w = (struct gmbus_wait_arg *)arg;

    w->status = i915_read(hw, GMBUS2);
    return (w->status & (GMBUS_SATOER | w->bit)) != 0;
}

/**
 * Wait for GMBUS2's `bit`, Linux's gmbus_wait, which waits on the GMBUS
 * interrupt where this polls: 0, -ENXIO for a NAK, or -ETIMEDOUT. May
 * sleep.
 */
static int32_t gmbus_wait(struct intel_gmbus *bus, uint32_t bit)
{
    struct gmbus_wait_arg w;

    w.bit = bit;
    w.status = 0;
    if (gmbus_poll(bus, "gmbus", gmbus_status_done, &w, GMBUS_WAIT_US) != 0) {
        return -ETIMEDOUT;
    }
    return (w.status & GMBUS_SATOER) ? -ENXIO : 0;
}

/** Whether GMBUS2 shows the controller no longer active. */
static int gmbus_idle_done(struct i915_hw *hw, void *arg)
{
    (void)arg;
    return (i915_read(hw, GMBUS2) & GMBUS_ACTIVE) == 0;
}

/** Wait for the controller to finish, Linux's gmbus_wait_idle: 0 or
 * -ETIMEDOUT. May sleep. */
static int32_t gmbus_wait_idle(struct intel_gmbus *bus)
{
    return gmbus_poll(bus, "gmbus idle", gmbus_idle_done, NULL, GMBUS_IDLE_US);
}

/** Read `len` bytes, 256 at most, from the sink at `addr` into `buf`,
 * Linux's gmbus_xfer_read_chunk; `index` is GMBUS1's index cycle, or 0. */
static int32_t gmbus_read_chunk(struct intel_gmbus *bus, uint32_t addr, uint8_t *buf, uint32_t len,
                                uint32_t index)
{
    struct i915_hw *hw = &bus->i915->hw;
    uint32_t val, loop;
    int32_t ret;

    i915_write(hw, GMBUS1,
               index | GMBUS_CYCLE_WAIT | (len << GMBUS_BYTE_COUNT_SHIFT) |
                   (addr << GMBUS_SLAVE_ADDR_SHIFT) | GMBUS_SLAVE_READ | GMBUS_SW_RDY);
    while (len) {
        ret = gmbus_wait(bus, GMBUS_HW_RDY);
        if (ret) {
            return ret;
        }
        val = i915_read(hw, GMBUS3);
        loop = 0;
        do {
            *buf++ = (uint8_t)(val & 0xff);
            val >>= 8;
        } while (--len && ++loop < 4);
    }
    return 0;
}

/** Read message `msg` in chunks of 256 bytes, Linux's gmbus_xfer_read. */
static int32_t gmbus_read(struct intel_gmbus *bus, const struct intel_i2c_msg *msg, uint32_t index)
{
    uint32_t left = msg->len, len;
    uint8_t *buf = msg->buf;
    int32_t ret;

    do {
        len = left < GMBUS_BYTE_COUNT_MAX ? left : GMBUS_BYTE_COUNT_MAX;
        ret = gmbus_read_chunk(bus, msg->addr, buf, len, index);
        if (ret) {
            return ret;
        }
        left -= len;
        buf += len;
    } while (left);
    return 0;
}

/** Write `len` bytes, 256 at most, of `buf` to the sink at `addr`, Linux's
 * gmbus_xfer_write_chunk: the first four before the command, the rest a
 * word at a time as the controller takes them. */
static int32_t gmbus_write_chunk(struct intel_gmbus *bus, uint32_t addr, const uint8_t *buf,
                                 uint32_t len, uint32_t index)
{
    struct i915_hw *hw = &bus->i915->hw;
    uint32_t size = len, val = 0, loop = 0;
    int32_t ret;

    while (len && loop < 4) {
        val |= (uint32_t)*buf++ << (8 * loop++);
        len--;
    }
    i915_write(hw, GMBUS3, val);
    i915_write(hw, GMBUS1,
               index | GMBUS_CYCLE_WAIT | (size << GMBUS_BYTE_COUNT_SHIFT) |
                   (addr << GMBUS_SLAVE_ADDR_SHIFT) | GMBUS_SLAVE_WRITE | GMBUS_SW_RDY);
    while (len) {
        val = loop = 0;
        do {
            val |= (uint32_t)*buf++ << (8 * loop);
        } while (--len && ++loop < 4);
        i915_write(hw, GMBUS3, val);
        ret = gmbus_wait(bus, GMBUS_HW_RDY);
        if (ret) {
            return ret;
        }
    }
    return 0;
}

/** Write message `msg` in chunks of 256 bytes, Linux's gmbus_xfer_write. */
static int32_t gmbus_write(struct intel_gmbus *bus, const struct intel_i2c_msg *msg, uint32_t index)
{
    uint32_t left = msg->len, len;
    const uint8_t *buf = msg->buf;
    int32_t ret;

    do {
        len = left < GMBUS_BYTE_COUNT_MAX ? left : GMBUS_BYTE_COUNT_MAX;
        ret = gmbus_write_chunk(bus, msg->addr, buf, len, index);
        if (ret) {
            return ret;
        }
        left -= len;
        buf += len;
    } while (left);
    return 0;
}

/** Whether messages `i` and `i + 1` make one indexed transfer, Linux's
 * gmbus_is_index_xfer: a write of one or two bytes of offset, then a
 * transfer at the same address. */
static int gmbus_is_index_xfer(const struct intel_i2c_msg *msgs, int i, int n)
{
    return i + 1 < n && msgs[i].addr == msgs[i + 1].addr && !msgs[i].read &&
           (msgs[i].len == 1 || msgs[i].len == 2) && msgs[i + 1].len > 0;
}

/** The indexed transfer of `msgs[0]` and `msgs[1]`, Linux's
 * gmbus_xfer_index_read: a one-byte offset in GMBUS1, a two-byte one in
 * GMBUS5 for the transfer's length. */
static int32_t gmbus_index_xfer(struct intel_gmbus *bus, const struct intel_i2c_msg *msgs)
{
    struct i915_hw *hw = &bus->i915->hw;
    uint32_t index = 0, gmbus5 = 0;
    int32_t ret;

    if (msgs[0].len == 2) {
        gmbus5 = GMBUS_2BYTE_INDEX_EN | msgs[0].buf[1] | ((uint32_t)msgs[0].buf[0] << 8);
    }
    if (msgs[0].len == 1) {
        index = GMBUS_CYCLE_INDEX | ((uint32_t)msgs[0].buf[0] << GMBUS_SLAVE_INDEX_SHIFT);
    }
    if (gmbus5) {
        i915_write(hw, GMBUS5, gmbus5);
    }
    ret = msgs[1].read ? gmbus_read(bus, &msgs[1], index) : gmbus_write(bus, &msgs[1], index);
    if (gmbus5) {
        i915_write(hw, GMBUS5, 0);
    }
    return ret;
}

/**
 * Transfer `n` messages through GMBUS, Linux's do_gmbus_xfer: the number of
 * messages, -ENXIO for a NAK, after one more try of a first message, or
 * -ETIMEDOUT for a controller that did not go idle after one, or -EAGAIN
 * for a controller that stopped answering, which the caller answers by
 * bit-banging.
 */
static int32_t do_gmbus_xfer(struct intel_gmbus *bus, struct intel_i2c_msg *msgs, int n)
{
    struct i915_hw *hw = &bus->i915->hw;
    int i = 0, inc, try = 0;
    int32_t ret = 0;

retry:
    i915_write(hw, GMBUS0, bus->reg0);
    for (; i < n; i += inc) {
        inc = 1;
        if (gmbus_is_index_xfer(msgs, i, n)) {
            ret = gmbus_index_xfer(bus, &msgs[i]);
            inc = 2;
        } else if (msgs[i].read) {
            ret = gmbus_read(bus, &msgs[i], 0);
        } else {
            ret = gmbus_write(bus, &msgs[i], 0);
        }
        if (!ret) {
            ret = gmbus_wait(bus, GMBUS_HW_WAIT_PHASE);
        }
        if (ret == -ETIMEDOUT) {
            goto timeout;
        } else if (ret) {
            goto clear_err;
        }
    }
    /* A STOP on the bus, then the controller idle and off. */
    i915_write(hw, GMBUS1, GMBUS_CYCLE_STOP | GMBUS_SW_RDY);
    ret = 0;
    if (gmbus_wait_idle(bus)) {
        klog(KLOG_DEBUG, "i915", "%s: timed out waiting for idle", bus->name);
        ret = -ETIMEDOUT;
    }
    i915_write(hw, GMBUS0, 0);
    return ret ? ret : i;

clear_err:
    /* The NAK clears only once the controller is idle; then the software
     * clear interrupt bit, set and cleared, readies it for the next
     * transfer. */
    bus->naks++;
    ret = -ENXIO;
    if (gmbus_wait_idle(bus)) {
        klog(KLOG_DEBUG, "i915", "%s: timed out after NAK", bus->name);
        ret = -ETIMEDOUT;
    }
    i915_write(hw, GMBUS1, GMBUS_SW_CLR_INT);
    i915_write(hw, GMBUS1, 0);
    i915_write(hw, GMBUS0, 0);
    klog(KLOG_DEBUG, "i915", "%s: NAK for addr %04x %c(%u)", bus->name,
         i < n ? (unsigned)msgs[i].addr : 0u, i < n && msgs[i].read ? 'r' : 'w',
         i < n ? (unsigned)msgs[i].len : 0u);
    /* A passive adapter may NAK the first probe. */
    if (ret == -ENXIO && i == 0 && try++ == 0) {
        klog(KLOG_DEBUG, "i915", "%s: NAK on first message, retry", bus->name);
        goto retry;
    }
    return ret;

timeout:
    bus->timeouts++;
    klog(KLOG_INFO, "i915", "%s: timed out, falling back to bit banging on pin %u", bus->name,
         (unsigned)(bus->reg0 & 0xff));
    i915_write(hw, GMBUS0, 0);
    return -EAGAIN;
}

/* ------------------------------------------------------------------------
 * Bit-banging through the GPIO pins, Linux's i2c-algo-bit
 */

/** The bits of the pin's GPIO register that a write keeps, Linux's
 * get_reserved: the pull-up disables. */
static uint32_t gpio_reserved(struct intel_gmbus *bus)
{
    return i915_read(&bus->i915->hw, bus->gpio_reg) &
           (GPIO_DATA_PULLUP_DIS | GPIO_CLOCK_PULLUP_DIS);
}

/** The level of the clock line, Linux's get_clock: the pin made an input,
 * then read. */
static int gpio_get_clock(struct intel_gmbus *bus)
{
    struct i915_hw *hw = &bus->i915->hw;
    uint32_t reserved = gpio_reserved(bus);

    i915_write(hw, bus->gpio_reg, reserved | GPIO_CLOCK_DIR_MASK);
    i915_write(hw, bus->gpio_reg, reserved);
    return (i915_read(hw, bus->gpio_reg) & GPIO_CLOCK_VAL_IN) != 0;
}

/** The level of the data line, Linux's get_data. */
static int gpio_get_data(struct intel_gmbus *bus)
{
    struct i915_hw *hw = &bus->i915->hw;
    uint32_t reserved = gpio_reserved(bus);

    i915_write(hw, bus->gpio_reg, reserved | GPIO_DATA_DIR_MASK);
    i915_write(hw, bus->gpio_reg, reserved);
    return (i915_read(hw, bus->gpio_reg) & GPIO_DATA_VAL_IN) != 0;
}

/** Let the clock line float high, or drive it low, Linux's set_clock. */
static void gpio_set_clock(struct intel_gmbus *bus, int high)
{
    struct i915_hw *hw = &bus->i915->hw;
    uint32_t bits = high ? GPIO_CLOCK_DIR_IN | GPIO_CLOCK_DIR_MASK
                         : GPIO_CLOCK_DIR_OUT | GPIO_CLOCK_DIR_MASK | GPIO_CLOCK_VAL_MASK;

    i915_write(hw, bus->gpio_reg, gpio_reserved(bus) | bits);
    i915_posting_read(hw, bus->gpio_reg);
}

/** Let the data line float high, or drive it low, Linux's set_data. */
static void gpio_set_data(struct intel_gmbus *bus, int high)
{
    struct i915_hw *hw = &bus->i915->hw;
    uint32_t bits = high ? GPIO_DATA_DIR_IN | GPIO_DATA_DIR_MASK
                         : GPIO_DATA_DIR_OUT | GPIO_DATA_DIR_MASK | GPIO_DATA_VAL_MASK;

    i915_write(hw, bus->gpio_reg, gpio_reserved(bus) | bits);
    i915_posting_read(hw, bus->gpio_reg);
}

/** Wait `us` microseconds on the device's clock. */
static void bit_delay(struct intel_gmbus *bus, uint32_t us)
{
    i915_delay_us(&bus->i915->hw, us);
}

/** Drive the data line low, Linux's sdalo. */
static void bit_sdalo(struct intel_gmbus *bus)
{
    gpio_set_data(bus, 0);
    bit_delay(bus, (BIT_UDELAY + 1) / 2);
}

/** Let the data line go high, Linux's sdahi. */
static void bit_sdahi(struct intel_gmbus *bus)
{
    gpio_set_data(bus, 1);
    bit_delay(bus, (BIT_UDELAY + 1) / 2);
}

/** Drive the clock line low, Linux's scllo. */
static void bit_scllo(struct intel_gmbus *bus)
{
    gpio_set_clock(bus, 0);
    bit_delay(bus, BIT_UDELAY / 2);
}

/** Let the clock line go high and wait while the sink stretches it,
 * Linux's sclhi: 0, or -ETIMEDOUT. */
static int32_t bit_sclhi(struct intel_gmbus *bus)
{
    uint32_t waited = 0;

    gpio_set_clock(bus, 1);
    while (!gpio_get_clock(bus)) {
        if (waited >= BIT_TIMEOUT_US) {
            if (gpio_get_clock(bus)) {
                break;
            }
            return -ETIMEDOUT;
        }
        bit_delay(bus, 1);
        waited++;
    }
    bit_delay(bus, BIT_UDELAY);
    return 0;
}

/** A START, Linux's i2c_start. */
static void bit_start(struct intel_gmbus *bus)
{
    gpio_set_data(bus, 0);
    bit_delay(bus, BIT_UDELAY);
    bit_scllo(bus);
}

/** A repeated START, Linux's i2c_repstart. */
static void bit_repstart(struct intel_gmbus *bus)
{
    bit_sdahi(bus);
    (void)bit_sclhi(bus);
    gpio_set_data(bus, 0);
    bit_delay(bus, BIT_UDELAY);
    bit_scllo(bus);
}

/** A STOP, Linux's i2c_stop. */
static void bit_stop(struct intel_gmbus *bus)
{
    bit_sdalo(bus);
    (void)bit_sclhi(bus);
    gpio_set_data(bus, 1);
    bit_delay(bus, BIT_UDELAY);
}

/** Send byte `c`, most significant bit first, Linux's i2c_outb: 1 when the
 * sink acknowledged it, 0 when not, or -ETIMEDOUT. */
static int32_t bit_outb(struct intel_gmbus *bus, uint8_t c)
{
    int i, ack;

    for (i = 7; i >= 0; i--) {
        gpio_set_data(bus, (c >> i) & 1);
        bit_delay(bus, (BIT_UDELAY + 1) / 2);
        if (bit_sclhi(bus) < 0) {
            return -ETIMEDOUT;
        }
        bit_scllo(bus);
    }
    bit_sdahi(bus);
    if (bit_sclhi(bus) < 0) {
        return -ETIMEDOUT;
    }
    ack = !gpio_get_data(bus);
    bit_scllo(bus);
    return ack;
}

/** Receive a byte, Linux's i2c_inb: the byte, or -ETIMEDOUT. */
static int32_t bit_inb(struct intel_gmbus *bus)
{
    uint32_t indata = 0;
    int i;

    bit_sdahi(bus);
    for (i = 0; i < 8; i++) {
        if (bit_sclhi(bus) < 0) {
            return -ETIMEDOUT;
        }
        indata *= 2;
        if (gpio_get_data(bus)) {
            indata |= 1;
        }
        gpio_set_clock(bus, 0);
        bit_delay(bus, i == 7 ? BIT_UDELAY / 2 : BIT_UDELAY);
    }
    return (int32_t)indata;
}

/** Acknowledge a byte received, or not for the last, Linux's acknak: 0,
 * or -ETIMEDOUT. */
static int32_t bit_acknak(struct intel_gmbus *bus, int ack)
{
    if (ack) {
        gpio_set_data(bus, 0);
    }
    bit_delay(bus, (BIT_UDELAY + 1) / 2);
    if (bit_sclhi(bus) < 0) {
        return -ETIMEDOUT;
    }
    bit_scllo(bus);
    return 0;
}

/** Send the address byte `addr`, again after a STOP and a START if the
 * sink did not acknowledge it, Linux's try_address. */
static int32_t bit_try_address(struct intel_gmbus *bus, uint8_t addr)
{
    int32_t ret = 0;
    int i;

    for (i = 0; i <= BIT_RETRIES; i++) {
        ret = bit_outb(bus, addr);
        if (ret == 1 || i == BIT_RETRIES) {
            break;
        }
        bit_stop(bus);
        bit_delay(bus, BIT_UDELAY);
        bit_start(bus);
    }
    return ret;
}

/** One message's bytes after its address, Linux's readbytes and sendbytes:
 * 0; -EIO for a read that stops short or a write the target does not
 * acknowledge, which counts in `naks`; or the bus's error. */
static int32_t bit_msg(struct intel_gmbus *bus, struct intel_i2c_msg *msg)
{
    int32_t b;
    uint32_t k;

    for (k = 0; k < msg->len; k++) {
        if (msg->read) {
            /* Linux's readbytes stops short, which reads as -EIO. */
            if ((b = bit_inb(bus)) < 0) {
                return -EIO;
            }
            msg->buf[k] = (uint8_t)b;
            if ((b = bit_acknak(bus, k + 1 < msg->len)) < 0) {
                return b;
            }
            continue;
        }
        b = bit_outb(bus, msg->buf[k]);
        if (b == 0) {
            bus->naks++;
        }
        if (b <= 0) {
            return b < 0 ? b : -EIO;
        }
    }
    return 0;
}

/**
 * Transfer `n` messages by bit-banging, Linux's bit_xfer with the GPIO
 * buses' pre_xfer and post_xfer: the number of messages, -ENXIO for an
 * address not acknowledged, -EIO for a byte not acknowledged, or
 * -ETIMEDOUT for a clock held low.
 */
static int32_t bit_xfer(struct intel_gmbus *bus, struct intel_i2c_msg *msgs, int n)
{
    int32_t ret = 0;
    int i;

    gmbus_reset(bus->i915);
    gpio_set_data(bus, 1);
    gpio_set_clock(bus, 1);
    bit_delay(bus, BIT_UDELAY);
    bit_start(bus);
    for (i = 0; i < n; i++) {
        if (i) {
            bit_repstart(bus);
        }
        if (bit_try_address(bus, (uint8_t)((msgs[i].addr << 1) | (msgs[i].read ? 1 : 0))) != 1) {
            bus->naks++;
            ret = -ENXIO;
            goto bailout;
        }
        if ((ret = bit_msg(bus, &msgs[i])) != 0) {
            goto bailout;
        }
    }
    ret = i;
bailout:
    bit_stop(bus);
    gpio_set_data(bus, 1);
    gpio_set_clock(bus, 1);
    return ret;
}

/* ------------------------------------------------------------------------
 * The buses
 */

/**
 * Take the GMBUS controller and the pins' modes for a transfer, Linux's
 * gmbus_lock_bus, which takes gmbus.mutex (intel_gmbus.c:842-849 and :972 in
 * 6.12.115, made at :895): a mutex, since the transfer's waits sleep and
 * another probe, a system call's or a worker's, may run meanwhile. Taken under
 * the commit lock where the caller holds it, never the other way round. Process
 * context or boot.
 */
static void gmbus_lock(struct drm_i915_private *i915)
{
    mutex_lock(&i915->display.gmbus_mutex);
}

/** Let the controller go, Linux's gmbus_unlock_bus. */
static void gmbus_unlock(struct drm_i915_private *i915)
{
    mutex_unlock(&i915->display.gmbus_mutex);
}

/**
 * Transfer `n` messages on the bus `ctx`, Linux's gmbus_xfer under the I2C
 * core's bus lock and one retry of -EAGAIN: GMBUS, or bit-banging while
 * the bus is forced to it; a GMBUS timeout forces bit-banging until a
 * bit-banged transfer fails. May sleep.
 */
static int32_t gmbus_xfer(void *ctx, struct intel_i2c_msg *msgs, int n)
{
    struct intel_gmbus *bus = (struct intel_gmbus *)ctx;
    int32_t ret = -EAGAIN;
    int try;

    /* The device awake for the bus, as Linux's gmbus_xfer holds the
     * GMBUS power domain. */
    i915_rpm_get(bus->i915);
    gmbus_lock(bus->i915);
    for (try = 0; try < 2 && ret == -EAGAIN; try++) {
        if (bus->force_bit) {
            bus->bit_xfers++;
            ret = bit_xfer(bus, msgs, n);
            if (ret < 0) {
                bus->force_bit &= ~GMBUS_FORCE_BIT_RETRY;
            }
        } else {
            bus->xfers++;
            ret = do_gmbus_xfer(bus, msgs, n);
            if (ret == -EAGAIN) {
                bus->force_bit |= GMBUS_FORCE_BIT_RETRY;
            }
        }
    }
    gmbus_unlock(bus->i915);
    i915_rpm_put(bus->i915);
    return ret;
}

int intel_gmbus_is_valid_pin(int pin)
{
    return pin > GMBUS_PIN_DISABLED && pin < GMBUS_NUM_PINS;
}

void intel_gmbus_setup(struct drm_i915_private *i915)
{
    struct intel_gmbus *bus;
    int pin;

    for (pin = 0; pin < GMBUS_NUM_PINS; pin++) {
        bus = &i915->display.gmbus[pin];
        k_memset(bus, 0, sizeof(*bus));
        bus->i915 = i915;
        bus->pin = (uint8_t)pin;
        if (!intel_gmbus_is_valid_pin(pin)) {
            continue;
        }
        bus->valid = 1;
        bus->name = gmbus_pins[pin].name;
        bus->reg0 = (uint32_t)pin | GMBUS_RATE_100KHZ;
        bus->gpio_reg = GPIO(gmbus_pins[pin].gpio);
        bus->bus.xfer = gmbus_xfer;
        bus->bus.ctx = bus;
        bus->bus.name = bus->name;
    }
    mutex_init(&i915->display.gmbus_mutex, "gmbus");
    gmbus_reset(i915);
}

struct intel_i2c_bus *intel_gmbus_bus(struct drm_i915_private *i915, int pin)
{
    if (!intel_gmbus_is_valid_pin(pin) || !i915->display.gmbus[pin].valid) {
        return NULL;
    }
    return &i915->display.gmbus[pin].bus;
}

void intel_gmbus_force_bit(struct drm_i915_private *i915, int pin, int force)
{
    struct intel_gmbus *bus;

    if (!intel_gmbus_is_valid_pin(pin)) {
        return;
    }
    bus = &i915->display.gmbus[pin];
    gmbus_lock(i915);
    if (force) {
        bus->force_bit++;
    } else if (bus->force_bit & ~GMBUS_FORCE_BIT_RETRY) {
        bus->force_bit--;
    }
    gmbus_unlock(i915);
    klog(KLOG_DEBUG, "i915", "%s: %sabling bit-banging, force_bit %u", bus->name,
         bus->force_bit ? "en" : "dis", (unsigned)(bus->force_bit & ~GMBUS_FORCE_BIT_RETRY));
}

int intel_gmbus_is_forced_bit(struct drm_i915_private *i915, int pin)
{
    return intel_gmbus_is_valid_pin(pin) && i915->display.gmbus[pin].force_bit != 0;
}
