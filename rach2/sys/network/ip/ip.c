/*
 * ip.c - Internet Protocol (IPv4).
 *
 * The 20-byte header has the source and destination addresses, the upper
 * protocol number, a TTL, and a one's complement checksum over the header.
 * Multi-byte fields are big-endian on the wire; we keep IP addresses as
 * host-endian uint32_t (net_ip_addr = 0x0A00020F = 10.0.2.15) and convert at
 * the wire boundary.
 *
 * We do not implement fragment reassembly, so any fragment (MF set or a
 * nonzero fragment offset) is dropped.
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
 * Compute the one's complement checksum over `len` bytes of `buf`.
 *
 * Sums 16-bit big-endian words, folding each carry back into the low 16 bits,
 * and returns the one's complement of the result. An odd trailing byte is
 * padded with a zero low byte. A header that already has a valid checksum
 * sums to zero, so callers verify by checking the result against zero.
 */
uint16_t ip_checksum(const uint8_t *buf, uint16_t len)
{
    uint32_t sum = 0;
    uint16_t i = 0;

    /* Four bytes a turn into a 32-bit accumulator, which cannot overflow
     * before 65536 words, and one fold at the end. The fold inside the loop
     * this replaces made the sum cost a compare and a branch for every two
     * bytes, and every byte sent and received passes through here. */
    while ((uint32_t)len - i >= 4u) {
        sum += (uint32_t)(((uint32_t)buf[i] << 8) | buf[i + 1]) +
               (uint32_t)(((uint32_t)buf[i + 2] << 8) | buf[i + 3]);
        i = (uint16_t)(i + 4);
    }
    for (; i + 1 < len; i = (uint16_t)(i + 2)) {
        sum += (uint32_t)(((uint32_t)buf[i] << 8) | buf[i + 1]);
    }
    if (len & 1) { /* odd length: pad the last byte with zero */
        sum += (uint32_t)buf[len - 1] << 8;
    }
    while (sum >> 16) {
        sum = (sum & 0xFFFFu) + (sum >> 16);
    }

    return (uint16_t)(~sum & 0xFFFF);
}

/**
 * Validate one received IPv4 datagram and dispatch it to the upper protocol.
 *
 * Drops anything malformed or not for us: a short buffer, a non-4 version, an
 * IHL or total length that does not fit the frame, a fragment (we do no
 * reassembly), a bad header checksum, or a destination that is not our address.
 * Options are skipped using IHL; the payload handed to the upper layer starts
 * at the header end and runs to `total_len`.
 *
 * `from_loopback` is nonzero only for the ring `ip_loop_drain` feeds; a packet
 * off the wire (`from_loopback == 0`) whose source is our own address or a
 * `127/8` loopback address is a martian, a spoof no real peer sends, and is
 * dropped before any reply can be steered onto the loopback ring or emitted
 * carrying a loopback source. A genuine loopback datagram has our own
 * source by design, so the check is gated on the delivery path, not the value.
 */
void ip_rx(const uint8_t *data, uint16_t len, int from_loopback)
{
    uint8_t ihl, proto;
    uint16_t total_len, hdr_len, frag;
    uint32_t src_ip, dst_ip;
    int to_broadcast;

    if (len < IP_HDR_SIZE) {
        return;
    }

    /* Version must be 4. */
    if ((data[0] >> 4) != 4) {
        return;
    }

    /* IHL is in 32-bit words; minimum 5 (20 bytes). Options are skipped. */
    ihl = data[0] & 0x0F;
    hdr_len = (uint16_t)(ihl * 4);
    if (hdr_len < IP_HDR_SIZE || hdr_len > len) {
        return;
    }

    total_len = read16be(data + 2);
    if (total_len > len || total_len < hdr_len) {
        return;
    }

    /* Drop fragments: MF set (0x2000) or a nonzero offset ([12:0]). */
    frag = read16be(data + 6);
    if (frag & 0x3FFF) {
        return;
    }

    /* Verify the header checksum (a valid header sums to zero). */
    if (ip_checksum(data, hdr_len) != 0) {
        return;
    }

    dst_ip = read32be(data + 16);
    /* An unconfigured machine owns no address, and 0.0.0.0 is not one it owns:
       comparing against it would make every packet addressed to nobody look
       like a packet addressed to this machine. */
    to_broadcast = netdev_is_broadcast(dst_ip);
    if (!netdev_owns(dst_ip) && !to_broadcast) {
        return; /* not addressed to us */
    }

    src_ip = read32be(data + 12);

    /* Reject a martian source off the wire: our own address, or 127/8, from a
     * peer is a spoof (only a loopback delivery legitimately has these). */
    if (!from_loopback && (netdev_owns(src_ip) || (src_ip & 0xFF000000u) == 0x7F000000u)) {
        return;
    }

    proto = data[9];

    /* A broadcast is delivered to UDP and to nothing else. The two broadcasts
       are how a DHCP server answers a machine that has no address yet and how
       a renewal's refusal arrives, which is the whole reason they are accepted.
       Answering a broadcast echo request would make this machine one of the
       many that reply to one packet, at whatever address the sender claimed,
       and a broadcast TCP segment is not a connection anyone opened. */
    if (to_broadcast && proto != IP_PROTO_UDP) {
        return;
    }

    /* Raw sockets see the whole datagram, header included, before the
       kernel's own handler for the protocol, which still runs. */
    raw_rx(proto, data, total_len);

    switch (proto) {
    case IP_PROTO_ICMP:
        icmp_rx(src_ip, data + hdr_len, (uint16_t)(total_len - hdr_len));
        break;
    case IP_PROTO_UDP:
        udp_rx(src_ip, data + hdr_len, (uint16_t)(total_len - hdr_len));
        break;
    case IP_PROTO_TCP:
        tcp_rx(src_ip, dst_ip, data + hdr_len, (uint16_t)(total_len - hdr_len));
        break;
    default:
        break;
    }
}

/* Identification counter for outgoing packets. */
static uint16_t ip_id_counter = 0;

/* TX assembly buffer at file scope. */
static uint8_t ip_tx_buf[IP_HDR_SIZE + 1500];

/* The loopback ring: datagrams addressed to ourselves, copied whole and
 * delivered by the timer tick. head is the next slot to fill, tail the next to
 * deliver; both only grow, so head - tail is the queue depth and the indices
 * wrap through the modulo. A full ring drops the datagram, exactly as a full
 * NIC ring would.
 *
 * The ring holds a send window's worth of full frames, since a local TCP
 * connection fills it with exactly that before the first acknowledgement can
 * come back: at eight slots a local transfer of any size lost datagrams from
 * its first burst and stalled. */
/* The ring's slots. This must be a power of two: `loop_head` and `loop_tail`
 * are 32-bit counters that wrap, and a size that does not divide 2^32 moves
 * every slot's place at the wrap, which loses frames and delivers others
 * twice without refusing anything. 64 is also more than the 45 segments a
 * window's worth of data puts in the ring in one system call. */
#define LOOP_RING_SLOTS 64u
static uint8_t loop_ring[LOOP_RING_SLOTS][IP_HDR_SIZE + 1500];
static uint16_t loop_ring_len[LOOP_RING_SLOTS];

/* Datagrams the ring had no room for, and whether the first has been
 * logged. A line for every one of them is what wrapped the whole kernel log,
 * and the boot record with it, the first time a local transfer ran. */
static uint32_t loop_dropped;
/* The most the ring has ever held at once. The ring is sized above what one
 * system call can put in it, and this is what says by how much. */
static uint32_t loop_peak;
static int loop_drop_logged;

/* Nonzero while `ip_loop_drain` is running, so a delivery that sends a reply
 * onto the ring cannot drain it again from inside itself. */
static int loop_draining;
static uint32_t loop_head, loop_tail;

/**
 * Build an IPv4 datagram around `payload` and hand it to the link layer.
 *
 * A datagram addressed to ourselves never touches ARP or the wire: it is
 * copied onto the loopback ring for the timer tick to deliver (the socket
 * layer canonicalizes 127/8 to our one address, so the self test is exact).
 * Delivery is deferred because the stack's shared transmit buffers and
 * protocol state are single-threaded by the interrupt discipline (net.h);
 * handing the datagram straight back to ip_rx here would reenter TCP under
 * the sender's own call frame.
 *
 * Everything else routes on-subnet destinations directly and through the
 * gateway otherwise, then resolves the next hop's MAC by ARP. On an ARP miss
 * it sends a request and drops this packet; the caller retries once the cache
 * is warm. The header is emitted with DF set and no fragmentation, so payloads
 * over the 1500-byte MTU are refused, not split.
 *
 * The return value is contracted in ip.h: the conditions a caller must hear
 * about are errnos, and the ARP miss is not one of them.
 */
int ip_tx(uint32_t dst_ip, uint8_t proto, const uint8_t *payload, uint16_t plen, uint8_t ttl)
{
    uint16_t total_len;
    uint8_t dst_mac[6];
    uint32_t route_ip;
    struct netdev *out;

    if (plen > 1500) {
        klog(KLOG_WARN, "ip", "tx: payload of %d bytes is too large, refused", (int)plen);
        return -EMSGSIZE;
    }

    total_len = (uint16_t)(IP_HDR_SIZE + plen);

    /* The interface first: its address is the source this datagram carries,
     * and a reply comes back to that and to no other. A broadcast goes out
     * by the first interface, which is where a machine with one card sends
     * everything. */
    if (netdev_is_broadcast(dst_ip)) {
        out = netdev_first();
        route_ip = dst_ip;
    } else {
        out = netdev_route(dst_ip, &route_ip);
        if (!out) {
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
    /* The source is the address of the interface this datagram leaves by,
     * which is the only address a reply can come back to. */
    write32be(ip_tx_buf + 12, out ? out->ip : net_ip_addr);
    write32be(ip_tx_buf + 16, dst_ip);
    write16be(ip_tx_buf + 10, ip_checksum(ip_tx_buf, IP_HDR_SIZE));

    k_memcpy(ip_tx_buf + IP_HDR_SIZE, payload, plen);

    if (netdev_owns(dst_ip)) {
        uint32_t slot;

        if (loop_head - loop_tail == LOOP_RING_SLOTS) {
            loop_dropped++;
            if (!loop_drop_logged) {
                loop_drop_logged = 1;
                klog(KLOG_WARN, "ip",
                     "loopback: the ring is full, dropped; further drops are counted");
            }
            return -ENOBUFS;
        }
        slot = loop_head % LOOP_RING_SLOTS;
        k_memcpy(loop_ring[slot], ip_tx_buf, total_len);
        loop_ring_len[slot] = total_len;
        loop_head++;
        if (loop_head - loop_tail > loop_peak) {
            loop_peak = loop_head - loop_tail;
        }
        return 0;
    }

    /* A broadcast goes to every machine on the wire, and the wire's own
       broadcast address is what reaches it: there is nobody to ask for a
       hardware address, and asking the gateway to forward it would send one
       machine's broadcast to a router that has no business with it. */
    if (netconf_is_broadcast(dst_ip)) {
        static const uint8_t all_ones[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

        /* Only a datagram is ever broadcast. A connection is between two
           machines and a broadcast SYN is one machine asking every machine on
           the wire to answer; an echo request to a broadcast address is one
           packet answered by all of them, at whatever address the sender
           claimed. Whether a datagram may be broadcast at all is the socket's
           permission, checked before it gets here. */
        if (proto != IP_PROTO_UDP) {
            klog(KLOG_WARN, "ip", "tx: refused to broadcast a datagram that is not UDP");
            return -EOPNOTSUPP;
        }
        if (ethernet_tx(out, all_ones, ETH_TYPE_IP, ip_tx_buf, total_len) != NETDEV_TX_OK) {
            return -ENOBUFS;
        }
        return 0;
    }

    /* The address is not resolved yet: the request has gone out and this
       datagram is dropped, which is not an error a datagram sender is told
       about. The next one leaves resolved. */
    if (!arp_lookup(out, route_ip, dst_mac)) {
        arp_request(out, route_ip);
        return 0;
    }

    {
        int sent = ethernet_tx(out, dst_mac, ETH_TYPE_IP, ip_tx_buf, total_len);

        if (sent == ETHERNET_TX_TOOLONG) {
            return -EMSGSIZE; /* the frame's own limit, which no retry passes */
        }
        if (sent != NETDEV_TX_OK) {
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

/**
 * Deliver queued loopback datagrams, called from the 100 Hz timer IRQ.
 *
 * Runs with interrupts off, after any syscall-context sender has left its
 * net_lock critical section, so every delivery sees quiescent stack state:
 * the same discipline under which the NIC hands frames up. A delivery may
 * itself transmit (an ACK, an echo reply) and enqueue onto this ring; the
 * tail advances only after ip_rx returns, so such an enqueue can never
 * overwrite the datagram being read, and the budget bounds one tick's work
 * so a reply chain cannot starve the timer.
 */
static uint32_t loop_delivered[3];

uint32_t ip_loop_delivered(int from)
{
    return from >= 0 && from < 3 ? loop_delivered[from] : 0;
}

void ip_loop_drain_from(int from)
{
    unsigned budget = LOOP_RING_SLOTS * 4;

    if (loop_draining) {
        return; /* a reply built inside a delivery: the outer drain takes it */
    }
    loop_draining = 1;
    while (loop_tail != loop_head && budget--) {
        uint32_t slot = loop_tail % LOOP_RING_SLOTS;

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
