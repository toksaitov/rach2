/*
 * icmp6.c: ICMPv6, as icmp6.h describes it: echo requests answered, echo
 * replies passed to the kernel's ping, the errors this machine sends, Packet
 * Too Big taken for the path MTU, and the counts of what comes in.
 */

#include "../../lib/kerrno.h"
#include "../../lib/kstring.h"
#include "icmp6.h"
#include "ip6.h"
#include "nd6.h"
#include "mld6.h"
#include "../ip/icmp.h"
#include "../core/byteorder.h"
#include "../driver/netdev.h"
#include "../../mm/kheap.h"
#include "../../timer.h"

/* The echo reply being sent when it fits one 1500-byte datagram; a larger
 * one is built in a block of the heap. */
static uint8_t icmp6_reply_buf[IP6_MTU - IP6_HDR_SIZE];

/* An error quotes as much of the datagram it answers as keeps the error
 * within the minimum MTU, 1280 bytes (RFC 4443, 2.4 (c)). */
#define ICMP6_QUOTE_MAX (IP6_MTU_MIN - IP6_HDR_SIZE - 8)
static uint8_t icmp6_error_buf[8 + ICMP6_QUOTE_MAX];

/* Errors leave at most one a second after a burst of six, as IPv4's do
 * (icmp.c), kept as a credit in ticks. */
#define ICMP6_ERROR_TICKS TICK_HZ
#define ICMP6_ERROR_BURST (6 * ICMP6_ERROR_TICKS)
static uint32_t icmp6_error_credit = ICMP6_ERROR_BURST;
static uint32_t icmp6_error_last;

/** Whether the rate limit lets an error leave now, taking its credit if
 * so: the credit grows by a tick each tick, up to the burst, and an error
 * costs a second of it. */
static int icmp6_error_allowed(void)
{
    uint32_t now = (uint32_t)kernel_ticks;
    uint32_t credit = icmp6_error_credit + (now - icmp6_error_last);

    if (credit > ICMP6_ERROR_BURST || credit < icmp6_error_credit) {
        credit = ICMP6_ERROR_BURST;
    }
    icmp6_error_last = now;
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
    } else if (type == ICMP6_TIME_EXCEEDED) {
        net_mib6.icmp6_out_time_excds++;
    } else if (type == ICMP6_PARAM_PROB) {
        net_mib6.icmp6_out_parm_problems++;
    } else if (type == ICMP6_ECHO_REQUEST) {
        net_mib6.icmp6_out_echos++;
    } else if (type == ICMP6_ECHO_REPLY) {
        net_mib6.icmp6_out_echo_replies++;
    } else if (type == ND6_ROUTER_SOLICIT) {
        net_mib6.icmp6_out_router_solicits++;
    } else if (type == ND6_NEIGHBOR_SOLICIT) {
        net_mib6.icmp6_out_neighbor_solicits++;
    } else if (type == ND6_NEIGHBOR_ADVERT) {
        net_mib6.icmp6_out_neighbor_advertisements++;
    } else if (type == ICMP6_MLD_REPORT) {
        net_mib6.icmp6_out_group_memb_responses++;
    } else if (type == ICMP6_MLD_DONE) {
        net_mib6.icmp6_out_group_memb_reductions++;
    } else if (type == ICMP6_MLD_V2_REPORT) {
        net_mib6.icmp6_out_mldv2_reports++;
    }
}

/**
 * The source is chosen before the checksum, which covers it, by the same
 * route `ip6_tx` then takes, and with the same answers when there is none.
 */
int icmp6_send(const uint8_t *src, const uint8_t *dst, int ifindex, uint8_t *msg, uint16_t len,
               uint8_t hlim)
{
    return icmp6_send_opts(src, dst, ifindex, msg, len, hlim, 0);
}

int icmp6_send_opts(const uint8_t *src, const uint8_t *dst, int ifindex, uint8_t *msg, uint16_t len,
                    uint8_t hlim, int opts)
{
    uint8_t from[IP6_ADDR_LEN], via[IP6_ADDR_LEN];
    int r;

    if (opts & IP6_TXF_UNSPEC) {
        src = ip6_any;
    } else if (!src || ip6_is_unspecified(src)) {
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
    r = ip6_tx_opts(src, dst, ifindex, IP6_NH_ICMP6, msg, len, hlim, opts);
    if (r == 0) {
        icmp6_count_out(msg[0]);
    }
    return r;
}

/**
 * The rules of RFC 4443, 2.4 (e): an error about an error could answer
 * itself without end, and one to a group or for a datagram sent to a group
 * would be answered by every member at once. Parameter problems of code 2
 * and packet too big are the exceptions the RFC allows for a group; this
 * machine sends the first, from the source `ip6_source` chooses, since a
 * group is no source. A datagram this machine sent itself is answered from
 * the loopback ring without the limit.
 */
void icmp6_error(struct netdev *nd, const uint8_t *dgram, uint16_t len, uint8_t type, uint8_t code,
                 uint32_t param)
{
    const uint8_t *src = dgram + 8;
    const uint8_t *dst = dgram + 24;
    uint16_t quote = len < ICMP6_QUOTE_MAX ? len : (uint16_t)ICMP6_QUOTE_MAX;

    if (ip6_is_unspecified(src) || ip6_is_multicast(src)) {
        return;
    }
    if (ip6_is_multicast(dst) && (type != ICMP6_PARAM_PROB || code != ICMP6_PARAM_OPTION)) {
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
    icmp6_send(ip6_is_multicast(dst) ? 0 : dst, src, nd ? nd->index : 0, icmp6_error_buf,
               (uint16_t)(8 + quote), IP6_HOP_LIMIT_DEFAULT);
}

/**
 * Take the Packet Too Big of `len` bytes at `msg`, received on `nd`: its
 * MTU is the 32-bit field after the checksum, and the datagram it quotes
 * follows. Only a datagram this machine sent, from ::1 or an address of its
 * own, has a path whose MTU it may lower. A destination link-scoped is
 * taken in the scope of the interface the message came by, or for the
 * loopback ring the one that holds the quoted source.
 */
static void icmp6_too_big(struct netdev *nd, const uint8_t *msg, uint16_t len)
{
    const uint8_t *q = msg + 8;
    const uint8_t *qsrc = q + 8, *qdst = q + 24;
    struct netdev *holder;
    int ifindex = 0;

    if (len < 8 + IP6_HDR_SIZE || (q[0] >> 4) != 6) {
        return;
    }
    holder = netdev_owner6(qsrc);
    if (!ip6_is_loopback(qsrc) && !holder) {
        return;
    }
    if (nd && (ip6_is_loopback(qdst) || (!ip6_is_multicast(qdst) && netdev_owner6(qdst)))) {
        return;
    }
    if (nd) {
        ifindex = nd->index;
    } else if (holder) {
        ifindex = holder->index;
    }
    ip6_pmtu_lower(qdst, ifindex, read32be(msg + 4));
}

/** The error goes from the sender's own address to itself, so it takes no
 * share of the limit on errors to other hosts. */
void icmp6_unreach_local(const uint8_t *dgram, uint16_t len)
{
    uint16_t quote = len < ICMP6_QUOTE_MAX ? len : (uint16_t)ICMP6_QUOTE_MAX;

    if (len < IP6_HDR_SIZE || ip6_is_unspecified(dgram + 8) || ip6_is_multicast(dgram + 8)) {
        return;
    }
    if (dgram[6] == IP6_NH_ICMP6 && len > IP6_HDR_SIZE && dgram[IP6_HDR_SIZE] < 128) {
        return;
    }
    icmp6_error_buf[0] = ICMP6_DST_UNREACH;
    icmp6_error_buf[1] = ICMP6_DST_ADDR;
    write32be(icmp6_error_buf + 4, 0);
    k_memcpy(icmp6_error_buf + 8, dgram, quote);
    icmp6_send(0, dgram + 8, 0, icmp6_error_buf, (uint16_t)(8 + quote), IP6_HOP_LIMIT_DEFAULT);
}

/**
 * An echo request is copied whole, its type changed to reply; one sent to a
 * group is answered from this machine's own address on the link it came
 * by, as Linux answers it, and goes back by that link.
 */
void icmp6_rx(struct netdev *nd, const uint8_t *src, const uint8_t *dst, uint8_t hlim,
              const uint8_t *data, uint16_t len)
{
    uint8_t *reply;

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
        icmp6_too_big(nd, data, len);
        break;
    case ICMP6_TIME_EXCEEDED:
        net_mib6.icmp6_in_time_excds++;
        break;
    case ICMP6_PARAM_PROB:
        net_mib6.icmp6_in_parm_problems++;
        break;
    case ICMP6_ECHO_REQUEST:
        net_mib6.icmp6_in_echos++;
        if (len < 8) {
            net_mib6.icmp6_in_errors++;
            break;
        }
        reply = len <= sizeof(icmp6_reply_buf) ? icmp6_reply_buf : kmalloc(len);
        if (!reply) {
            break;
        }
        k_memcpy(reply, data, len);
        reply[0] = ICMP6_ECHO_REPLY;
        reply[1] = 0;
        icmp6_send(ip6_is_multicast(dst) ? 0 : dst, src, nd ? nd->index : 0, reply, len,
                   IP6_HOP_LIMIT_DEFAULT);
        if (reply != icmp6_reply_buf) {
            kfree(reply);
        }
        break;
    case ICMP6_ECHO_REPLY:
        net_mib6.icmp6_in_echo_replies++;
        if (len >= 8) {
            icmp_ping_reply6(src, read16be(data + 4), read16be(data + 6), len, hlim);
        }
        break;
    case ND6_ROUTER_SOLICIT:
    case ND6_ROUTER_ADVERT:
    case ND6_NEIGHBOR_SOLICIT:
    case ND6_NEIGHBOR_ADVERT:
    case ND6_REDIRECT:
        nd6_rx(nd, src, dst, hlim, data, len);
        break;
    case ICMP6_MLD_QUERY:
    case ICMP6_MLD_REPORT:
    case ICMP6_MLD_DONE:
    case ICMP6_MLD_V2_REPORT:
        mld6_rx(nd, src, dst, hlim, data, len);
        break;
    default:
        break;
    }
}
