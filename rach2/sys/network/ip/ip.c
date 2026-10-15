/*
 * ip.c: the IPv4 layer, as ip.h describes it: the checks of a received
 * datagram, the transmit and its choice of interface, the datagrams held for
 * ARP, and the loopback ring. The 20-byte header has the source and
 * destination addresses, the protocol number, a TTL, and a one's complement
 * checksum over the header, its multi-byte fields big-endian.
 */

#include "../../klog.h"
#include "../../lib/kerrno.h"
#include "ip.h"
#include "icmp.h"
#include "../udp/udp.h"
#include "../tcp/tcp.h"
#include "../arp/arp.h"
#include "../core/byteorder.h"
#include "../ethernet/ethernet.h"
#include "../driver/netdev.h"
#include "../socket/raw.h"
#include "../net.h"
#include "../netconf.h"
#include "../../serial.h"
#include "../../lib/kstring.h"

/**
 * The words are summed four bytes a turn into a 32-bit accumulator, which
 * cannot overflow before 65536 words, and the carries are folded back into
 * the low 16 bits once at the end.
 */
uint16_t ip_checksum(const uint8_t *buf, uint16_t len)
{
    uint32_t sum = 0;
    uint16_t i = 0;

    while ((uint32_t)len - i >= 4u) {
        sum += (uint32_t)(((uint32_t)buf[i] << 8) | buf[i + 1]) +
               (uint32_t)(((uint32_t)buf[i + 2] << 8) | buf[i + 3]);
        i = (uint16_t)(i + 4);
    }
    for (; i + 1 < len; i = (uint16_t)(i + 2)) {
        sum += (uint32_t)(((uint32_t)buf[i] << 8) | buf[i + 1]);
    }
    if (len & 1) {
        sum += (uint32_t)buf[len - 1] << 8;
    }
    while (sum >> 16) {
        sum = (sum & 0xFFFFu) + (sum >> 16);
    }

    return (uint16_t)(~sum & 0xFFFF);
}

/**
 * A broadcast is given to UDP and to nothing else: a DHCP server may answer
 * a machine that has no address yet by broadcast, and a renewal's refusal
 * arrives as one. Answering a broadcast echo request would make this machine
 * one of the many that reply to one packet, at whatever address the sender
 * claimed, and a broadcast TCP segment is not a connection anyone opened.
 *
 * A datagram from the wire whose source is an address of this machine or in
 * 127/8 is a martian, a spoof no peer sends, and is dropped before any reply
 * can be steered onto the loopback ring or sent with a loopback source. A
 * datagram on the loopback ring has this machine's source by design, so the
 * check depends on the path the datagram came by and not on its source. One
 * from the wire to 127/8 is dropped too, so that a socket bound to 127.0.0.1
 * is reachable from this machine alone.
 */
void ip_rx(const uint8_t *data, uint16_t len, int from_loopback)
{
    uint8_t ihl, proto;
    uint16_t total_len, hdr_len, frag;
    uint32_t src_ip, dst_ip;
    int to_broadcast;

    net_mib.ip_in_receives++;
    if (len < IP_HDR_SIZE) {
        net_mib.ip_in_hdr_errors++;
        return;
    }

    if ((data[0] >> 4) != 4) {
        net_mib.ip_in_hdr_errors++;
        return;
    }

    /* IHL counts 32-bit words; 5, 20 bytes, is a header with no options. */
    ihl = data[0] & 0x0F;
    hdr_len = (uint16_t)(ihl * 4);
    if (hdr_len < IP_HDR_SIZE || hdr_len > len) {
        net_mib.ip_in_hdr_errors++;
        return;
    }

    total_len = read16be(data + 2);
    if (total_len > len || total_len < hdr_len) {
        net_mib.ip_in_hdr_errors++;
        return;
    }

    /* MF is bit 13 of the field, 0x2000, and the fragment offset bits 12-0. */
    frag = read16be(data + 6);
    if (frag & 0x3FFF) {
        net_mib.ip_in_discards++;
        return;
    }

    if (ip_checksum(data, hdr_len) != 0) {
        net_mib.ip_in_hdr_errors++;
        return;
    }

    dst_ip = read32be(data + 16);
    to_broadcast = netdev_is_broadcast(dst_ip);
    if (!netdev_owns(dst_ip) && !to_broadcast && !(from_loopback && IP_IS_LOOPBACK(dst_ip))) {
        if ((dst_ip & 0xF0000000u) != 0xE0000000u) {
            net_mib.ip_in_addr_errors++;
        }
        return;
    }

    src_ip = read32be(data + 12);

    if (!from_loopback && (netdev_owns(src_ip) || (src_ip & 0xFF000000u) == 0x7F000000u)) {
        net_mib.ip_in_addr_errors++;
        return;
    }

    proto = data[9];

    if (to_broadcast && proto != IP_PROTO_UDP) {
        net_mib.ip_in_discards++;
        return;
    }

    raw_rx(proto, data, total_len);

    switch (proto) {
    case IP_PROTO_ICMP:
        net_mib.ip_in_delivers++;
        icmp_rx(src_ip, dst_ip, data[8], data + hdr_len, (uint16_t)(total_len - hdr_len));
        break;
    case IP_PROTO_UDP:
        net_mib.ip_in_delivers++;
        if (udp_rx(src_ip, dst_ip, data + hdr_len, (uint16_t)(total_len - hdr_len)) != 0 &&
            !to_broadcast) {
            icmp_unreachable(data, total_len, ICMP_PORT_UNREACH);
        }
        break;
    case IP_PROTO_TCP:
        net_mib.ip_in_delivers++;
        tcp_rx(src_ip, dst_ip, data + hdr_len, (uint16_t)(total_len - hdr_len));
        break;
    default:
        net_mib.ip_in_unknown_protos++;
        break;
    }
}

/* The identification field of the next datagram sent. */
static uint16_t ip_id_counter = 0;

/* TX assembly buffer at file scope. */
static uint8_t ip_tx_buf[IP_HDR_SIZE + 1500];

extern volatile uint32_t kernel_ticks;

/* Datagrams that wait for their next hop's hardware address, as Linux's
 * neighbour queue keeps them, so that the first datagram to a host not yet in
 * the ARP cache leaves when the reply comes and is not lost: a ping's first
 * request, a connect's SYN. A slot is free when its length is 0, or once its
 * datagram has waited longer than any reply takes, IP_HOLD_TICKS, three
 * seconds; `since` is the tick the datagram was put in. */
#define IP_HOLD_SLOTS 4
#define IP_HOLD_TICKS 300
static struct ip_hold {
    struct netdev *out;
    uint32_t hop;
    uint32_t since;
    uint16_t len;
    uint8_t datagram[1500];
} ip_hold[IP_HOLD_SLOTS];

/**
 * Keep the datagram of `len` bytes in `ip_tx_buf` for `hop` on `out`, in a
 * free slot, or in place of the one held longest when none is free. A
 * datagram put out of its slot counts in `arp_stats`: as `res_failed` when
 * it was past its wait, as `unres_discards` when it was not.
 */
static void ip_hold_put(struct netdev *out, uint32_t hop, uint16_t len)
{
    struct ip_hold *h = &ip_hold[0];
    int i;

    for (i = 0; i < IP_HOLD_SLOTS; i++) {
        struct ip_hold *c = &ip_hold[i];

        if (c->len == 0 || (uint32_t)(kernel_ticks - c->since) > IP_HOLD_TICKS) {
            h = c;
            break;
        }
        if ((int32_t)(c->since - h->since) < 0) {
            h = c;
        }
    }
    if (h->len != 0 && (uint32_t)(kernel_ticks - h->since) > IP_HOLD_TICKS) {
        arp_stats.res_failed++;
    } else if (h->len != 0) {
        arp_stats.unres_discards++;
    }
    h->out = out;
    h->hop = hop;
    h->since = kernel_ticks;
    h->len = len;
    k_memcpy(h->datagram, ip_tx_buf, len);
}

void ip_hold_resolved(struct netdev *nd, uint32_t hop, const uint8_t *mac)
{
    int i;

    for (i = 0; i < IP_HOLD_SLOTS; i++) {
        struct ip_hold *h = &ip_hold[i];

        if (h->len != 0 && h->out == nd && h->hop == hop) {
            if ((uint32_t)(kernel_ticks - h->since) <= IP_HOLD_TICKS) {
                ethernet_tx(nd, mac, ETH_TYPE_IP, h->datagram, h->len);
            }
            h->len = 0;
        }
    }
}

void ip_hold_forget(struct netdev *nd)
{
    int i;

    for (i = 0; i < IP_HOLD_SLOTS; i++) {
        if (!nd || ip_hold[i].out == nd) {
            ip_hold[i].len = 0;
        }
    }
}

/* The loopback ring: datagrams addressed to this machine, copied whole and
 * delivered by `ip_loop_drain_from`, from the timer tick, the idle wait, or a
 * system call's exit. `loop_head` is the next slot to fill and `loop_tail`
 * the next to deliver; both only grow, so their difference is the queue's
 * depth and a slot is the counter modulo the ring's size. A full ring
 * refuses the datagram, as a full card's ring would.
 *
 * The slots are a power of two: the two counters are 32 bits and wrap, and a
 * size that does not divide 2^32 moves every slot's place at the wrap, which
 * loses frames and delivers others twice without refusing anything. 64 is
 * more than the 45 full segments of a send window, which a local TCP
 * connection puts in the ring in one system call, before the first
 * acknowledgement can come back. */
#define LOOP_RING_SLOTS 64u
static uint8_t loop_ring[LOOP_RING_SLOTS][IP_HDR_SIZE + 1500];
static uint16_t loop_ring_len[LOOP_RING_SLOTS];

/* Datagrams the ring had no room for. Only the first drop is logged, as
 * `loop_drop_logged` records: a line for each would wrap the kernel log and
 * lose the boot record with it. */
static uint32_t loop_dropped;
/* The most the ring has ever held at once, which shows how far the ring's
 * size is above what one system call can put in it. */
static uint32_t loop_peak;
static int loop_drop_logged;

/* Nonzero while `ip_loop_drain_from` is running, so a delivery that sends a
 * reply onto the ring cannot drain it again from inside itself. */
static int loop_draining;
static uint32_t loop_head, loop_tail;

/**
 * The interface a broadcast to `dst` leaves by: the card whose subnet `dst`
 * is the broadcast address of, or the first card for the limited broadcast,
 * 255.255.255.255, which names no card, and for an address no card's subnet
 * has. The limited broadcast is judged first, since it is also the subnet
 * broadcast of a card on 128.0.0.0/1.
 */
static struct netdev *ip_broadcast_iface(uint32_t dst)
{
    int i;

    if (dst == 0xFFFFFFFFu) {
        return netdev_first();
    }
    for (i = 0; i < netdev_count(); i++) {
        struct netdev *nd = netdev_at(i);

        int a;

        if (nd && nd->ip != 0 && nd->mask != 0 && dst == (nd->ip | ~nd->mask)) {
            return nd;
        }
        for (a = 0; nd && a < nd->naddr2; a++) {
            if (dst == (nd->ip2[a] | ~nd->mask2[a])) {
                return nd;
            }
        }
    }
    return netdev_first();
}

/**
 * Put the datagram of `total_len` bytes in `ip_tx_buf` on the loopback ring,
 * counted as `lo`'s traffic, as Linux counts it: 0, or -ENOBUFS when the
 * ring is full, which is counted and logged once.
 */
static int loop_put(uint16_t total_len)
{
    uint32_t slot;

    if (loop_head - loop_tail == LOOP_RING_SLOTS) {
        net_mib.ip_out_discards++;
        loop_dropped++;
        if (!loop_drop_logged) {
            loop_drop_logged = 1;
            klog(KLOG_WARN, "ip", "loopback: the ring is full, dropped; further drops are counted");
        }
        return -ENOBUFS;
    }
    slot = loop_head % LOOP_RING_SLOTS;
    k_memcpy(loop_ring[slot], ip_tx_buf, total_len);
    loop_ring_len[slot] = total_len;
    loop_head++;
    if (netdev_loopback()) {
        netdev_loopback()->tx_packets++;
        netdev_loopback()->tx_bytes += total_len;
    }
    if (loop_head - loop_tail > loop_peak) {
        loop_peak = loop_head - loop_tail;
    }
    return 0;
}

int ip_is_local(uint32_t ip)
{
    return IP_IS_LOOPBACK(ip) || netdev_owns(ip);
}

uint32_t ip_source(uint32_t dst)
{
    uint32_t via;
    struct netdev *out;

    if (IP_IS_LOOPBACK(dst)) {
        return IP_LOOPBACK_ADDR;
    }
    out = netdev_route(dst, &via);
    return out ? netdev_source(out, via) : net_ip_addr;
}

int ip_tx(uint32_t dst_ip, uint8_t proto, const uint8_t *payload, uint16_t plen, uint8_t ttl)
{
    return ip_tx_from(0, dst_ip, proto, payload, plen, ttl);
}

/**
 * The interface is chosen first, since its address is the datagram's source
 * and the only address a reply can come back to; a datagram to 127/8 needs
 * no route. A datagram to this machine never touches ARP or the wire, and
 * its delivery waits for the drain: the stack's shared transmit buffers and
 * protocol state depend on the interrupt discipline of net.h, and a datagram
 * given straight back to `ip_rx` here would enter TCP again inside the
 * sender's own call.
 *
 * A broadcast goes with no ARP and no gateway, and only for UDP: a broadcast
 * SYN is one machine asking every machine on the wire to answer, and a
 * broadcast echo request is one packet answered by all of them, at whatever
 * address the sender claimed. Whether a datagram may be broadcast at all is
 * the socket's permission, checked before it gets here. A datagram held for
 * ARP is reported sent, since a datagram lost when no reply comes is not an
 * error a sender is told about. A frame `ethernet_tx` finds too long is
 * EMSGSIZE, which no retry passes, and a frame the driver refused is
 * ENOBUFS.
 */
int ip_tx_from(uint32_t src, uint32_t dst_ip, uint8_t proto, const uint8_t *payload, uint16_t plen,
               uint8_t ttl)
{
    uint16_t total_len;
    uint8_t dst_mac[6];
    uint32_t route_ip;
    struct netdev *out;

    if (plen > 1500 - IP_HDR_SIZE) {
        klog(KLOG_WARN, "ip", "tx: payload of %d bytes is too large, refused", (int)plen);
        return -EMSGSIZE;
    }

    total_len = (uint16_t)(IP_HDR_SIZE + plen);
    net_mib.ip_out_requests++;

    if (IP_IS_LOOPBACK(dst_ip)) {
        out = 0;
        route_ip = dst_ip;
    } else if (IP_IS_LOOPBACK(src) && !netdev_owns(dst_ip)) {
        return -EINVAL;
    } else if (netdev_is_broadcast(dst_ip)) {
        out = ip_broadcast_iface(dst_ip);
        route_ip = dst_ip;
    } else {
        out = netdev_route(dst_ip, &route_ip);
        if (!out) {
            net_mib.ip_out_no_routes++;
            klog(KLOG_WARN, "ip", "tx: no route: refused a datagram no interface reaches");
            return -ENETUNREACH;
        }
    }

    ip_tx_buf[0] = 0x45; /* version 4, IHL 5 */
    ip_tx_buf[1] = 0x00; /* DSCP/ECN */
    write16be(ip_tx_buf + 2, total_len);
    write16be(ip_tx_buf + 4, ip_id_counter++);
    write16be(ip_tx_buf + 6, 0x4000); /* DF set, no fragments */
    ip_tx_buf[8] = ttl;               /* hop limit */
    ip_tx_buf[9] = proto;
    write16be(ip_tx_buf + 10, 0x0000); /* checksum, filled below */
    if (src == 0 || !ip_is_local(src)) {
        src = IP_IS_LOOPBACK(dst_ip) ? IP_LOOPBACK_ADDR
              : out                  ? netdev_source(out, route_ip)
                                     : net_ip_addr;
    }
    write32be(ip_tx_buf + 12, src);
    write32be(ip_tx_buf + 16, dst_ip);
    write16be(ip_tx_buf + 10, ip_checksum(ip_tx_buf, IP_HDR_SIZE));

    k_memcpy(ip_tx_buf + IP_HDR_SIZE, payload, plen);

    if (IP_IS_LOOPBACK(dst_ip) || netdev_owns(dst_ip)) {
        return loop_put(total_len);
    }

    if (netconf_is_broadcast(dst_ip)) {
        static const uint8_t all_ones[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

        if (proto != IP_PROTO_UDP) {
            klog(KLOG_WARN, "ip", "tx: refused to broadcast a datagram that is not UDP");
            return -EOPNOTSUPP;
        }
        if (ethernet_tx(out, all_ones, ETH_TYPE_IP, ip_tx_buf, total_len) != NETDEV_TX_OK) {
            net_mib.ip_out_discards++;
            return -ENOBUFS;
        }
        return 0;
    }

    if (!arp_lookup(out, route_ip, dst_mac)) {
        ip_hold_put(out, route_ip, total_len);
        arp_request(out, route_ip);
        return 0;
    }

    {
        int sent = ethernet_tx(out, dst_mac, ETH_TYPE_IP, ip_tx_buf, total_len);

        if (sent == ETHERNET_TX_TOOLONG) {
            return -EMSGSIZE;
        }
        if (sent != NETDEV_TX_OK) {
            net_mib.ip_out_discards++;
            return -ENOBUFS;
        }
    }
    return 0;
}

uint32_t ip_loop_dropped(void)
{
    return loop_dropped;
}

int ip_loop_pending(void)
{
    return loop_head != loop_tail;
}

uint32_t ip_loop_peak(void)
{
    return loop_peak;
}

/* More than a pair queued: a simultaneous open is a pair of crossing SYNs and
 * is only a pair while neither has been delivered, so a pair is left to the
 * timer; past that the queue is a transfer and is delivered at once. */
#define LOOP_DRAIN_AT 3u

int ip_loop_crowded(void)
{
    return loop_head - loop_tail >= LOOP_DRAIN_AT;
}

/* Datagrams delivered, counted by who drained them (`IP_LOOP_FROM_*`). */
static uint32_t loop_delivered[3];

uint32_t ip_loop_delivered(int from)
{
    return from >= 0 && from < 3 ? loop_delivered[from] : 0;
}

/**
 * Each caller has interrupts off and has left any `net_lock` bracket of its
 * own, so every delivery finds the stack at rest, as a card's interrupt
 * does. A delivery may itself send onto this ring (an acknowledgement, an
 * echo reply); the tail advances only after `ip_rx` returns, so such a send
 * never overwrites the datagram being read. The budget, four times the
 * ring's slots, bounds one call's work, so that a chain of replies cannot
 * hold the timer.
 */
void ip_loop_drain_from(int from)
{
    unsigned budget = LOOP_RING_SLOTS * 4;

    if (loop_draining) {
        return;
    }
    loop_draining = 1;
    while (loop_tail != loop_head && budget--) {
        uint32_t slot = loop_tail % LOOP_RING_SLOTS;

        if (netdev_loopback()) {
            netdev_loopback()->rx_packets++;
            netdev_loopback()->rx_bytes += loop_ring_len[slot];
        }
        ip_rx(loop_ring[slot], loop_ring_len[slot], 1);
        loop_tail++;
        if (from >= 0 && from < 3) {
            loop_delivered[from]++;
        }
    }
    loop_draining = 0;
}

void ip_loop_drain(void)
{
    ip_loop_drain_from(IP_LOOP_FROM_TICK);
}
