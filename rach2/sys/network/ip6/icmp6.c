/*
 * icmp6.c: ICMPv6, as icmp6.h describes it: echo requests answered, echo
 * replies passed to the kernel's ping, the errors this machine sends, and
 * the counts of what comes in.
 */

#include "../../lib/kerrno.h"
#include "../../lib/kstring.h"
#include "icmp6.h"
#include "ip6.h"
#include "../ip/icmp.h"
#include "../core/byteorder.h"
#include "../driver/netdev.h"

#include "../../timer.h"

/* The echo reply being sent: the largest message a 1500-byte datagram
 * holds. */
static uint8_t icmp6_reply_buf[IP6_MTU - IP6_HDR_SIZE];

/* An error quotes as much of the datagram it answers as keeps the error
 * within the minimum MTU, 1280 bytes (RFC 4443, 2.4 (c)). */
#define ICMP6_QUOTE_MAX (IP6_MTU_MIN - IP6_HDR_SIZE - 8)
static uint8_t icmp6_error_buf[8 + ICMP6_QUOTE_MAX];

/* Errors leave at most one a second after a burst of six, as IPv4's do
 * (icmp.c), kept as a credit in ticks. */
#define ICMP6_ERROR_TICKS 100
#define ICMP6_ERROR_BURST (6 * ICMP6_ERROR_TICKS)
static uint32_t icmp6_error_credit = ICMP6_ERROR_BURST;
static uint32_t icmp6_error_last;

/** Whether the rate limit lets an error leave now, taking its credit if
 * so: the credit grows by a tick each tick, up to the burst, and an error
 * costs a second of it. */
static int icmp6_error_allowed(void)
{
    uint32_t credit = icmp6_error_credit + (kernel_ticks - icmp6_error_last);

    if (credit > ICMP6_ERROR_BURST || credit < icmp6_error_credit) {
        credit = ICMP6_ERROR_BURST;
    }
    icmp6_error_last = kernel_ticks;
    if (credit < ICMP6_ERROR_TICKS) {
        icmp6_error_credit = credit;
        return 0;
    }
    icmp6_error_credit = credit - ICMP6_ERROR_TICKS;
    return 1;
}

/** Count an ICMPv6 message of type `type` sent. */
static void icmp6_count_out(uint8_t type)
{
    net_mib6.icmp6_out_msgs++;
    if (type == ICMP6_DST_UNREACH) {
        net_mib6.icmp6_out_dest_unreachs++;
    } else if (type == ICMP6_PARAM_PROB) {
        net_mib6.icmp6_out_parm_problems++;
    } else if (type == ICMP6_ECHO_REQUEST) {
        net_mib6.icmp6_out_echos++;
    } else if (type == ICMP6_ECHO_REPLY) {
        net_mib6.icmp6_out_echo_replies++;
    }
}

/**
 * The source is chosen before the checksum, which covers it, by the same
 * route `ip6_tx` then takes, and with the same answers when there is none.
 */
int icmp6_send(const uint8_t *src, const uint8_t *dst, int ifindex, uint8_t *msg, uint16_t len,
               uint8_t hlim)
{
    uint8_t from[IP6_ADDR_LEN], via[IP6_ADDR_LEN];
    int r;

    if (!src || ip6_is_unspecified(src)) {
        struct netdev *out = ip6_route(dst, ifindex, via);

        if (!out) {
            return ifindex == 0 && ip6_link_scoped(dst) && netdev_count() > 1 ? -EINVAL
                                                                              : -ENETUNREACH;
        }
        r = ip6_source(out, dst, from);
        if (r != 0) {
            return r;
        }
        src = from;
    }
    msg[2] = 0;
    msg[3] = 0;
    write16be(msg + 2, ip6_checksum(src, dst, IP6_NH_ICMP6, msg, len));
    r = ip6_tx(src, dst, ifindex, IP6_NH_ICMP6, msg, len, hlim);
    if (r == 0) {
        icmp6_count_out(msg[0]);
    }
    return r;
}

/**
 * The rules of RFC 4443, 2.4 (e): an error about an error could answer
 * itself without end, and one to a group or for a datagram sent to a group
 * would be answered by every member at once. Parameter problems of code 2
 * and packet too big are the exceptions the RFC allows for a group, and this
 * machine sends neither. A datagram this machine sent itself is answered
 * from the loopback ring without the limit.
 */
void icmp6_error(struct netdev *nd, const uint8_t *dgram, uint16_t len, uint8_t type, uint8_t code,
                 uint32_t param)
{
    const uint8_t *src = dgram + 8;
    const uint8_t *dst = dgram + 24;
    uint16_t quote = len < ICMP6_QUOTE_MAX ? len : (uint16_t)ICMP6_QUOTE_MAX;

    if (ip6_is_unspecified(src) || ip6_is_multicast(src) || ip6_is_multicast(dst)) {
        return;
    }
    if (dgram[6] == IP6_NH_ICMP6 && len > IP6_HDR_SIZE &&
        (dgram[IP6_HDR_SIZE] < 128 || dgram[IP6_HDR_SIZE] == 137)) {
        return;
    }
    if (nd && !icmp6_error_allowed()) {
        return;
    }
    icmp6_error_buf[0] = type;
    icmp6_error_buf[1] = code;
    write32be(icmp6_error_buf + 4, param);
    k_memcpy(icmp6_error_buf + 8, dgram, quote);
    icmp6_send(dst, src, nd ? nd->index : 0, icmp6_error_buf, (uint16_t)(8 + quote),
               IP6_HOP_LIMIT_DEFAULT);
}

/**
 * An echo request is copied whole, its type changed to reply; one sent to a
 * group is answered from this machine's own address on the link it came
 * by, as Linux answers it, and goes back by that link.
 */
void icmp6_rx(struct netdev *nd, const uint8_t *src, const uint8_t *dst, uint8_t hlim,
              const uint8_t *data, uint16_t len)
{
    net_mib6.icmp6_in_msgs++;
    if (len < 4 || ip6_checksum(src, dst, IP6_NH_ICMP6, data, len) != 0) {
        net_mib6.icmp6_in_errors++;
        return;
    }
    switch (data[0]) {
    case ICMP6_DST_UNREACH:
        net_mib6.icmp6_in_dest_unreachs++;
        break;
    case ICMP6_PACKET_TOO_BIG:
        net_mib6.icmp6_in_pkt_too_bigs++;
        break;
    case ICMP6_TIME_EXCEEDED:
        net_mib6.icmp6_in_time_excds++;
        break;
    case ICMP6_PARAM_PROB:
        net_mib6.icmp6_in_parm_problems++;
        break;
    case ICMP6_ECHO_REQUEST:
        net_mib6.icmp6_in_echos++;
        if (len < 8 || len > sizeof(icmp6_reply_buf)) {
            net_mib6.icmp6_in_errors++;
            break;
        }
        k_memcpy(icmp6_reply_buf, data, len);
        icmp6_reply_buf[0] = ICMP6_ECHO_REPLY;
        icmp6_reply_buf[1] = 0;
        icmp6_send(ip6_is_multicast(dst) ? 0 : dst, src, nd ? nd->index : 0, icmp6_reply_buf, len,
                   IP6_HOP_LIMIT_DEFAULT);
        break;
    case ICMP6_ECHO_REPLY:
        net_mib6.icmp6_in_echo_replies++;
        if (len >= 8) {
            icmp_ping_reply6(src, read16be(data + 4), read16be(data + 6), len, hlim);
        }
        break;
    default:
        break;
    }
}
