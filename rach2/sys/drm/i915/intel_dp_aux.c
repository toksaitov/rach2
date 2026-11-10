/*
 * intel_dp_aux.c: the AUX channel of the eDP panel on DDI A, after Linux's
 * display/intel_dp_aux.c for Haswell, with the DPCD reads and writes and
 * the I2C over AUX of the EDID that Linux's drm_dp_helper.c builds on it.
 */

#include "i915.h"
#include "i915_reg.h"
#include "intel_dp.h"

#include "../../klog.h"
#include "../../lib/kerrno.h"
#include "../../lib/kstring.h"

/* Linux's bounds: three looks at a busy channel 1 ms apart, 10 ms for a
 * message to finish, five sends of each, and the DPCD's 32 tries 500 us
 * apart (drm_dp_dpcd_access). */
#define AUX_BUSY_TRIES   3
#define AUX_DONE_US      10000u
#define AUX_SEND_TRIES   5
#define AUX_RETRY_US     500u
#define AUX_RX_ERROR_US  400u
#define DPCD_TRIES       32
#define I2C_TRIES        7
#define I2C_DEFER_TRIES  7
#define EDID_BLOCK_TRIES 4
#define DDC_TRIES        5

/* The extra precharge of a message in 2 us units: Linux's sync length,
 * 16 us of precharge and 16 of preamble, less the 10 us and the preamble
 * the hardware sends itself. */
#define AUX_PRECHARGE_2US ((16u + 16u - 10u - 16u) / 2u)

/* A message's header, and the bare address of one without data. */
#define AUX_HEADER_SIZE       4
#define AUX_BARE_ADDRESS_SIZE 3

/**
 * The AUX clock divider of try `index`: the CD clock for port A's channel,
 * the raw clock for the PCH's, divided to 2 MHz, Linux's
 * ilk_get_aux_clock_divider; 0 ends the list. Haswell's non-ULT
 * workaround, 63 and 72 for the PCH's channels, does not apply to the
 * ULT's Lynx Point LP.
 */
static uint32_t aux_divider(struct drm_i915_private *i915, int port, int index)
{
    uint32_t freq;

    if (index != 0) {
        return 0;
    }
    freq = port == 0 ? i915->display.cdclk
                     : (i915_read(&i915->hw, PCH_RAWCLK_FREQ) & RAWCLK_FREQ_MASK) * 1000u;
    return (freq + 1000u) / 2000u;
}

/** Pack up to four bytes of `src` into a data register, the first byte
 * highest, Linux's intel_dp_aux_pack. */
static uint32_t aux_pack(const uint8_t *src, int n)
{
    uint32_t v = 0;
    int i;

    for (i = 0; i < n && i < 4; i++) {
        v |= (uint32_t)src[i] << ((3 - i) * 8);
    }
    return v;
}

/** Unpack up to four bytes of a data register into `dst`. */
static void aux_unpack(uint32_t v, uint8_t *dst, int n)
{
    int i;

    for (i = 0; i < n && i < 4; i++) {
        dst[i] = (uint8_t)(v >> ((3 - i) * 8));
    }
}

/**
 * Send `send_bytes` bytes of `send` on the panel's AUX channel and take up
 * to `recv_size` bytes of the reply into `recv`, Linux's
 * intel_dp_aux_xfer: the panel's VDD held for the transfer, a channel
 * still busy waited for three times, each send tried five times while it
 * times out or meets a receive error, which costs 400 us.
 *
 * @return the bytes received, or -EBUSY for a channel that stayed busy, a
 *         send never done, or a reply of 0 or more than 20 bytes, -EIO for
 *         a receive error and -ETIMEDOUT when no sink answered.
 */
static int32_t aux_xfer(struct drm_i915_private *i915, const uint8_t *send, int send_bytes,
                        uint8_t *recv, int recv_size)
{
    struct i915_hw *hw = &i915->hw;
    int port = i915->display.dp.port;
    uint32_t ctl = DP_AUX_CH_CTL(port), status = 0, divider, send_ctl;
    int32_t ret;
    int i, try, clock = 0, vdd, recv_bytes;

    vdd = intel_pps_vdd_on_unlocked(i915);
    for (try = 0; try < AUX_BUSY_TRIES; try++) {
        status = i915_read(hw, ctl);
        if (!(status & DP_AUX_CH_CTL_SEND_BUSY)) {
            break;
        }
        intel_display_msleep(i915, 1);
    }
    if (try == AUX_BUSY_TRIES) {
        klog(KLOG_WARN, "i915", "aux: not started: status 0x%08x", (unsigned)status);
        ret = -EBUSY;
        goto out;
    }
    if (send_bytes > 20 || recv_size > 20) {
        ret = -E2BIG;
        goto out;
    }
    while ((divider = aux_divider(i915, port, clock++)) != 0) {
        send_ctl = DP_AUX_CH_CTL_SEND_BUSY | DP_AUX_CH_CTL_DONE | DP_AUX_CH_CTL_INTERRUPT |
                   DP_AUX_CH_CTL_TIME_OUT_ERROR | DP_AUX_CH_CTL_TIME_OUT_400us |
                   DP_AUX_CH_CTL_RECEIVE_ERROR | DP_AUX_CH_CTL_MESSAGE_SIZE(send_bytes) |
                   DP_AUX_CH_CTL_PRECHARGE_2US(AUX_PRECHARGE_2US) | divider;
        for (try = 0; try < AUX_SEND_TRIES; try++) {
            for (i = 0; i < send_bytes; i += 4) {
                i915_write(hw, DP_AUX_CH_DATA(port, i >> 2), aux_pack(send + i, send_bytes - i));
            }
            i915_write(hw, ctl, send_ctl);
            if (i915_wait_for(hw, "aux done", ctl, DP_AUX_CH_CTL_SEND_BUSY, 0, AUX_DONE_US) != 0) {
                klog(KLOG_ERR, "i915", "aux: did not complete within 10 ms");
            }
            status = i915_read(hw, ctl);
            i915_write(hw, ctl,
                       status | DP_AUX_CH_CTL_DONE | DP_AUX_CH_CTL_TIME_OUT_ERROR |
                           DP_AUX_CH_CTL_RECEIVE_ERROR);
            if (status & DP_AUX_CH_CTL_TIME_OUT_ERROR) {
                continue; /* the timeout itself was the 400 us the spec asks */
            }
            if (status & DP_AUX_CH_CTL_RECEIVE_ERROR) {
                i915_delay_us(hw, AUX_RX_ERROR_US);
                continue;
            }
            if (status & DP_AUX_CH_CTL_DONE) {
                goto done;
            }
        }
    }
    if (!(status & DP_AUX_CH_CTL_DONE)) {
        klog(KLOG_ERR, "i915", "aux: not done: status 0x%08x", (unsigned)status);
        ret = -EBUSY;
        goto out;
    }
done:
    if (status & DP_AUX_CH_CTL_RECEIVE_ERROR) {
        klog(KLOG_ERR, "i915", "aux: receive error: status 0x%08x", (unsigned)status);
        ret = -EIO;
        goto out;
    }
    if (status & DP_AUX_CH_CTL_TIME_OUT_ERROR) {
        ret = -ETIMEDOUT;
        goto out;
    }
    recv_bytes = (int)((status & DP_AUX_CH_CTL_MESSAGE_SIZE_MASK) >> 20);
    if (recv_bytes == 0 || recv_bytes > 20) {
        ret = -EBUSY;
        goto out;
    }
    if (recv_bytes > recv_size) {
        recv_bytes = recv_size;
    }
    for (i = 0; i < recv_bytes; i += 4) {
        aux_unpack(i915_read(hw, DP_AUX_CH_DATA(port, i >> 2)), recv + i, recv_bytes - i);
    }
    ret = recv_bytes;
out:
    if (vdd) {
        intel_pps_vdd_off_unlocked(i915, 0);
    }
    return ret;
}

int32_t intel_dp_aux_transfer(struct drm_i915_private *i915, struct intel_dp_aux_msg *msg)
{
    uint8_t tx[20], rx[20];
    int txsize, rxsize;
    int32_t ret;

    tx[0] = (uint8_t)((msg->request << 4) | ((msg->address >> 16) & 0xfu));
    tx[1] = (uint8_t)(msg->address >> 8);
    tx[2] = (uint8_t)msg->address;
    tx[3] = (uint8_t)(msg->size - 1u);
    switch (msg->request & ~DP_AUX_I2C_MOT) {
    case DP_AUX_NATIVE_WRITE:
    case DP_AUX_I2C_WRITE:
    case DP_AUX_I2C_WRITE_STATUS_UPDATE:
        txsize = msg->size ? AUX_HEADER_SIZE + (int)msg->size : AUX_BARE_ADDRESS_SIZE;
        rxsize = 2; /* the reply, and a short write's count */
        if (txsize > 20) {
            return -E2BIG;
        }
        if (msg->size) {
            k_memcpy(tx + AUX_HEADER_SIZE, msg->buffer, msg->size);
        }
        ret = aux_xfer(i915, tx, txsize, rx, rxsize);
        if (ret > 0) {
            msg->reply = (uint8_t)(rx[0] >> 4);
            ret = ret > 1 ? (rx[1] < msg->size ? rx[1] : (int32_t)msg->size) : (int32_t)msg->size;
        }
        return ret;
    case DP_AUX_NATIVE_READ:
    case DP_AUX_I2C_READ:
        txsize = msg->size ? AUX_HEADER_SIZE : AUX_BARE_ADDRESS_SIZE;
        rxsize = (int)msg->size + 1;
        if (rxsize > 20) {
            return -E2BIG;
        }
        ret = aux_xfer(i915, tx, txsize, rx, rxsize);
        if (ret > 0) {
            msg->reply = (uint8_t)(rx[0] >> 4);
            ret--;
            k_memcpy(msg->buffer, rx + 1, (size_t)ret);
        }
        return ret;
    default:
        return -EINVAL;
    }
}

/**
 * A native transfer of `size` bytes at DPCD `addr`, Linux's
 * drm_dp_dpcd_access: up to 32 tries, 500 us apart but after a timeout,
 * until the sink acknowledges the whole. 0 is `size`; the first error of
 * the tries otherwise: -EPROTO for a short reply, -EIO for a NAK or a
 * DEFER, or the transfer's.
 */
static int32_t dpcd_access(struct drm_i915_private *i915, uint8_t request, uint32_t addr,
                           uint8_t *buf, uint32_t size)
{
    struct intel_dp_aux_msg msg;
    int32_t ret = 0, err = 0;
    int retry;

    k_memset(&msg, 0, sizeof(msg));
    msg.address = addr;
    msg.request = request;
    msg.buffer = buf;
    msg.size = size;
    for (retry = 0; retry < DPCD_TRIES; retry++) {
        if (ret != 0 && ret != -ETIMEDOUT) {
            i915_delay_us(&i915->hw, AUX_RETRY_US);
        }
        ret = intel_dp_aux_transfer(i915, &msg);
        if (ret >= 0) {
            if ((msg.reply & DP_AUX_NATIVE_REPLY_MASK) == DP_AUX_NATIVE_REPLY_ACK) {
                if (ret == (int32_t)size) {
                    return ret;
                }
                ret = -EPROTO;
            } else {
                ret = -EIO;
            }
        }
        if (err == 0) {
            err = ret;
        }
    }
    return err;
}

int32_t intel_dp_dpcd_read(struct drm_i915_private *i915, uint32_t addr, uint8_t *buf,
                           uint32_t size)
{
    uint8_t rev;
    int32_t ret;

    /* Linux reads DPCD_REV first: some sinks corrupt the first access
     * after they leave power saving. */
    ret = dpcd_access(i915, DP_AUX_NATIVE_READ, DP_DPCD_REV, &rev, 1);
    if (ret < 0) {
        return ret;
    }
    return dpcd_access(i915, DP_AUX_NATIVE_READ, addr, buf, size);
}

int32_t intel_dp_dpcd_write(struct drm_i915_private *i915, uint32_t addr, const uint8_t *buf,
                            uint32_t size)
{
    uint8_t copy[DP_AUX_MAX_PAYLOAD_BYTES];

    if (size > sizeof(copy)) {
        return -E2BIG;
    }
    k_memcpy(copy, buf, size);
    return dpcd_access(i915, DP_AUX_NATIVE_WRITE, addr, copy, size);
}

int32_t intel_dp_dpcd_writeb(struct drm_i915_private *i915, uint32_t addr, uint8_t value)
{
    return intel_dp_dpcd_write(i915, addr, &value, 1);
}

/* ------------------------------------------------------------------------
 * I2C over AUX, Linux's drm_dp_i2c_xfer
 */

/* An I2C message: `len` bytes of `buf` to or from the device at `addr`. */
struct i2c_msg {
    uint8_t addr;
    uint8_t read;
    uint32_t len;
    uint8_t *buf;
};

/** The AUX request of `m`, with middle-of-transaction set, since no
 * message of the EDID's ends with a stop of its own. */
static uint8_t i2c_request(const struct i2c_msg *m)
{
    return (uint8_t)((m->read ? DP_AUX_I2C_READ : DP_AUX_I2C_WRITE) | DP_AUX_I2C_MOT);
}

/**
 * One I2C over AUX message, Linux's drm_dp_i2c_do_msg: retried while the
 * channel is busy and the sink defers, natively or on its I2C side, up to
 * seven times and seven more for I2C defers. The bytes the sink took or
 * gave, or -EREMOTEIO for a NAK and too many defers, or the transfer's
 * error.
 */
static int32_t i2c_do_msg(struct drm_i915_private *i915, struct intel_dp_aux_msg *msg)
{
    int retry, defer_i2c = 0;
    int32_t ret;

    for (retry = 0; retry < I2C_TRIES + defer_i2c; retry++) {
        ret = intel_dp_aux_transfer(i915, msg);
        if (ret < 0) {
            if (ret == -EBUSY) {
                continue;
            }
            return ret;
        }
        switch (msg->reply & DP_AUX_NATIVE_REPLY_MASK) {
        case DP_AUX_NATIVE_REPLY_ACK:
            break;
        case DP_AUX_NATIVE_REPLY_DEFER:
            i915_delay_us(&i915->hw, AUX_RETRY_US);
            continue;
        default:
            return -EREMOTEIO;
        }
        switch (msg->reply & DP_AUX_I2C_REPLY_MASK) {
        case DP_AUX_I2C_REPLY_ACK:
            if (ret != (int32_t)msg->size && (msg->request & ~DP_AUX_I2C_MOT) == DP_AUX_I2C_WRITE) {
                msg->request =
                    (uint8_t)((msg->request & DP_AUX_I2C_MOT) | DP_AUX_I2C_WRITE_STATUS_UPDATE);
            }
            return ret;
        case DP_AUX_I2C_REPLY_DEFER:
            if (defer_i2c < I2C_DEFER_TRIES) {
                defer_i2c++;
            }
            i915_delay_us(&i915->hw, AUX_RETRY_US);
            if ((msg->request & ~DP_AUX_I2C_MOT) == DP_AUX_I2C_WRITE) {
                msg->request =
                    (uint8_t)((msg->request & DP_AUX_I2C_MOT) | DP_AUX_I2C_WRITE_STATUS_UPDATE);
            }
            continue;
        default:
            return -EREMOTEIO;
        }
    }
    return -EREMOTEIO;
}

/** Send or take the whole of `orig`, a piece at a time while the sink
 * answers short, Linux's drm_dp_i2c_drain_msg: the shortest answer, or an
 * error. */
static int32_t i2c_drain(struct drm_i915_private *i915, const struct intel_dp_aux_msg *orig)
{
    struct intel_dp_aux_msg msg = *orig;
    int32_t err, ret = (int32_t)orig->size;

    while (msg.size > 0) {
        err = i2c_do_msg(i915, &msg);
        if (err <= 0) {
            return err == 0 ? -EPROTO : err;
        }
        if (err < (int32_t)msg.size && err < ret) {
            ret = err;
        }
        msg.size -= (uint32_t)err;
        msg.buffer += err;
    }
    return ret;
}

/**
 * The I2C transaction of `n` messages `msgs` over AUX, Linux's
 * drm_dp_i2c_xfer: each message started by a bare address, sent in pieces
 * of up to 16 bytes, fewer once the sink answers short, and the
 * transaction closed by a bare address without middle-of-transaction.
 * `n`, or the first error.
 */
static int32_t i2c_xfer(struct drm_i915_private *i915, struct i2c_msg *msgs, int n)
{
    struct intel_dp_aux_msg msg;
    uint32_t j, transfer = DP_AUX_MAX_PAYLOAD_BYTES;
    int32_t err = 0;
    int i;

    k_memset(&msg, 0, sizeof(msg));
    for (i = 0; i < n; i++) {
        msg.address = msgs[i].addr;
        msg.request = i2c_request(&msgs[i]);
        msg.buffer = NULL;
        msg.size = 0;
        err = i2c_do_msg(i915, &msg);
        msg.request = i2c_request(&msgs[i]);
        if (err < 0) {
            break;
        }
        transfer = DP_AUX_MAX_PAYLOAD_BYTES;
        for (j = 0; j < msgs[i].len; j += msg.size) {
            msg.buffer = msgs[i].buf + j;
            msg.size = msgs[i].len - j < transfer ? msgs[i].len - j : transfer;
            err = i2c_drain(i915, &msg);
            msg.request = i2c_request(&msgs[i]);
            if (err < 0) {
                break;
            }
            transfer = (uint32_t)err;
        }
        if (err < 0) {
            break;
        }
    }
    if (err >= 0) {
        err = n;
    }
    msg.request &= (uint8_t)~DP_AUX_I2C_MOT;
    msg.buffer = NULL;
    msg.size = 0;
    (void)i2c_do_msg(i915, &msg);
    return err;
}

/**
 * Read EDID block `block` into `buf`, Linux's drm_do_probe_ddc_edid: the
 * segment pointer for blocks past the first two, the offset, and the
 * block's 128 bytes, five times at most. 0, or -EIO.
 */
static int32_t ddc_block(struct drm_i915_private *i915, uint32_t block, uint8_t *buf)
{
    uint8_t start = (uint8_t)(block * 128u), segment = (uint8_t)(block >> 1);
    int xfers = segment ? 3 : 2, retries = DDC_TRIES;
    int32_t ret;

    do {
        struct i2c_msg msgs[3];

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
        msgs[2].len = 128;
        msgs[2].buf = buf;
        ret = i2c_xfer(i915, &msgs[3 - xfers], xfers);
    } while (ret != xfers && --retries);
    return ret == xfers ? 0 : -EIO;
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

int32_t intel_dp_read_edid(struct drm_i915_private *i915, uint8_t *buf, uint32_t cap, uint32_t *len)
{
    uint32_t i, blocks;
    int try;

    *len = 0;
    if (cap < 128) {
        return -EINVAL;
    }
    for (try = 0; try < EDID_BLOCK_TRIES; try++) {
        if (ddc_block(i915, 0, buf) != 0) {
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
        klog(KLOG_WARN, "i915", "edp: EDID of %u blocks, %u kept", (unsigned)blocks,
             (unsigned)(cap / 128u));
        blocks = cap / 128u;
    }
    for (i = 1; i < blocks; i++) {
        for (try = 0; try < EDID_BLOCK_TRIES; try++) {
            if (ddc_block(i915, i, buf + i * 128u) != 0) {
                return -EIO;
            }
            if (edid_block_ok(buf + i * 128u, 0)) {
                break;
            }
        }
        if (try == EDID_BLOCK_TRIES) {
            /* Linux drops a bad extension; the ones before it stand. */
            klog(KLOG_WARN, "i915", "edp: EDID block %u bad, %u kept", (unsigned)i, (unsigned)i);
            break;
        }
    }
    *len = i * 128u;
    return 0;
}
