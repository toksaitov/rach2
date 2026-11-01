/*
 * ip.c: the IPv4 layer, as ip.h describes it: the checks of a received
 * datagram, the transmit, its choice of interface and its fragments, the
 * datagrams held for ARP, and the loopback ring; ipfrag.c reassembles. The 20-byte header has the
 * source and destination addresses, the protocol number, a TTL, and a one's complement checksum
 * over the header, its multi-byte fields big-endian.
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
#include "../ip6/ip6.h"
#include "../net.h"
#include "../netconf.h"
#include "../../proc/ns.h"
#include "../../serial.h"
#include "../../lib/kstring.h"
#include "../../mm/kheap.h"
#include "../../timer.h"

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
 * is reachable from this machine alone. A fragment is judged by the same
 * checks as a whole datagram, each by its own header, before it is kept.
 */
void ip_rx(const uint8_t *data, uint16_t len, int from_loopback)
{
    uint8_t ihl, proto;
    uint16_t total_len, hdr_len;
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

    if (read16be(data + 6) & (IP_MF | IP_FRAG_OFFSET)) {
        ip_frag_input(data, total_len, hdr_len, to_broadcast);
        return;
    }
    ip_deliver(data, total_len, to_broadcast);
}

void ip_deliver(const uint8_t *data, uint16_t total_len, int to_broadcast)
{
    uint16_t hdr_len = (uint16_t)((data[0] & 0x0F) * 4);
    uint32_t src_ip = read32be(data + 12);
    uint32_t dst_ip = read32be(data + 16);
    uint8_t proto = data[9];

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

/* The datagram, or the fragment, being sent: one frame's payload. */
static uint8_t ip_tx_buf[ETH_MTU];

/* The fragment being sent of a held datagram whose hop's address came. */
static uint8_t ip_hold_frag_buf[ETH_MTU];

/* Datagrams that wait for their next hop's hardware address, as Linux's
 * neighbour queue keeps them, so that the first datagram to a host not yet in
 * the ARP cache leaves when the reply comes and is not lost: a ping's first
 * request, a connect's SYN. A datagram past the MTU is held whole, before
 * fragmentation, and cut into fragments of `mtu` bytes when it leaves, so
 * that the 119 fragments of the largest at 576 bytes take one slot and no
 * fragment of it can put out another. A slot is free when its length is 0,
 * or once its datagram has waited longer than any reply takes,
 * IP_HOLD_TICKS, three seconds; `since` is the tick the datagram was put in.
 * A held datagram is a copy on the heap, its header included, freed when it
 * leaves or is dropped. */
#define IP_HOLD_SLOTS 64
#define IP_HOLD_TICKS MS_TO_TICKS(3000)
static struct ip_hold {
    struct netdev *out;
    uint32_t hop;
    uint32_t since;
    uint16_t len;
    uint32_t mtu; /* the MTU its fragments are cut to, 0 to leave whole */
    uint8_t *datagram;
} ip_hold[IP_HOLD_SLOTS];

static int ip_fragment_to(struct netdev *out, const uint8_t *mac, const uint8_t *hdr,
                          const uint8_t *payload, uint16_t plen, uint32_t mtu, uint8_t *buf);

/** Free the datagram held in `h` and free the slot. */
static void ip_hold_free(struct ip_hold *h)
{
    kfree(h->datagram);
    h->datagram = 0;
    h->len = 0;
}

/**
 * Keep the datagram of the IP header `hdr` and `plen` bytes at `payload`
 * for `hop` on `out`, in a free slot, or in place of the one held longest
 * when none is free; `mtu` is the MTU its fragments are cut to when it
 * leaves, or 0 for one that leaves whole. A datagram put out of its slot
 * counts in `arp_stats`: as `res_failed` when it was past its wait, as
 * `unres_discards` when it was not. A datagram no memory holds is dropped,
 * as a send promises no delivery.
 */
static void ip_hold_put(struct netdev *out, uint32_t hop, const uint8_t *hdr,
                        const uint8_t *payload, uint16_t plen, uint32_t mtu)
{
    uint16_t len = (uint16_t)(IP_HDR_SIZE + plen);
    uint8_t *copy = kmalloc(len);

    struct ip_hold *h = &ip_hold[0];
    int i;

    for (i = 0; i < IP_HOLD_SLOTS; i++) {
        struct ip_hold *c = &ip_hold[i];

        if (c->len == 0 || ((uint32_t)kernel_ticks - c->since) > IP_HOLD_TICKS) {
            h = c;
            break;
        }
        if ((int32_t)(c->since - h->since) < 0) {
            h = c;
        }
    }
    if (h->len != 0 && ((uint32_t)kernel_ticks - h->since) > IP_HOLD_TICKS) {
        arp_stats.res_failed++;
    } else if (h->len != 0) {
        arp_stats.unres_discards++;
    }
    ip_hold_free(h);
    if (!copy) {
        net_mib.ip_out_discards++;
        return;
    }
    k_memcpy(copy, hdr, IP_HDR_SIZE);
    k_memcpy(copy + IP_HDR_SIZE, payload, plen);
    h->out = out;
    h->hop = hop;
    h->since = (uint32_t)kernel_ticks;
    h->len = len;
    h->mtu = mtu;
    h->datagram = copy;
}

void ip_hold_resolved(struct netdev *nd, uint32_t hop, const uint8_t *mac)
{
    int i;

    for (i = 0; i < IP_HOLD_SLOTS; i++) {
        struct ip_hold *h = &ip_hold[i];

        if (h->len != 0 && h->out == nd && h->hop == hop) {
            if (((uint32_t)kernel_ticks - h->since) > IP_HOLD_TICKS) {
                /* past its wait: dropped */
            } else if (h->mtu) {
                ip_fragment_to(nd, mac, h->datagram, h->datagram + IP_HDR_SIZE,
                               (uint16_t)(h->len - IP_HDR_SIZE), h->mtu, ip_hold_frag_buf);
            } else {
                ethernet_tx(nd, mac, ETH_TYPE_IP, h->datagram, h->len);
            }
            ip_hold_free(h);
        }
    }
}

void ip_hold_forget(struct netdev *nd)
{
    int i;

    for (i = 0; i < IP_HOLD_SLOTS; i++) {
        if (!nd || ip_hold[i].out == nd) {
            ip_hold_free(&ip_hold[i]);
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
 * acknowledgement can come back. A slot holds a datagram of one frame; a
 * larger one, up to `lo`'s MTU, is kept on the heap, in `loop_ring_big`,
 * until it is delivered. */
#define LOOP_RING_SLOTS 64u
static uint8_t loop_ring[LOOP_RING_SLOTS][ETH_MTU];
static uint8_t *loop_ring_big[LOOP_RING_SLOTS];
/* A slot's length, 0 for a datagram dropped on the ring (`ip_loop_ns_gone`). */
static uint32_t loop_ring_len[LOOP_RING_SLOTS];
/* The card an IPv6 datagram is delivered as received by, or 0 for `lo`
 * (`ip_loop_put_on`). */
static int loop_ring_if[LOOP_RING_SLOTS];
/* The network namespace a slot's datagram is delivered in, `ip_ns` when it
 * was put there. */
static const struct netns *loop_ring_ns[LOOP_RING_SLOTS];

const struct netns *ip_ns;

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

int ip_loop_put(const uint8_t *dgram, uint32_t total_len)
{
    return ip_loop_put_parts(dgram, total_len, 0, 0, 0);
}

int ip_loop_put_on(const uint8_t *dgram, uint32_t total_len, int ifindex)
{
    return ip_loop_put_parts(dgram, total_len, 0, 0, ifindex);
}

/** Count a datagram the ring had no room for, logging the first. */
static int loop_drop(void)
{
    loop_dropped++;
    if (!loop_drop_logged) {
        loop_drop_logged = 1;
        klog(KLOG_WARN, "ip", "loopback: the ring is full, dropped; further drops are counted");
    }
    return -ENOBUFS;
}

int ip_loop_put_parts(const uint8_t *hdr, uint32_t hlen, const uint8_t *payload, uint32_t plen,
                      int ifindex)
{
    uint32_t slot, total_len = hlen + plen;
    struct netdev *lo;
    uint8_t *at;

    if (total_len > NETDEV_LOOPBACK_MTU) {
        return -EMSGSIZE;
    }
    if (loop_head - loop_tail == LOOP_RING_SLOTS) {
        return loop_drop();
    }
    slot = loop_head % LOOP_RING_SLOTS;
    at = loop_ring[slot];
    if (total_len > sizeof(loop_ring[0])) {
        at = kmalloc(total_len);
        if (!at) {
            return loop_drop();
        }
    }
    loop_ring_big[slot] = at != loop_ring[slot] ? at : 0;
    k_memcpy(at, hdr, hlen);
    if (plen) {
        k_memcpy(at + hlen, payload, plen);
    }
    loop_ring_len[slot] = total_len;
    loop_ring_if[slot] = ifindex;
    loop_ring_ns[slot] = ip_ns;
    loop_head++;
    lo = ip_ns ? ip_ns->lo : netdev_loopback();
    if (lo) {
        lo->tx_packets++;
        lo->tx_bytes += total_len;
    }
    if (loop_head - loop_tail > loop_peak) {
        loop_peak = loop_head - loop_tail;
    }
    return 0;
}

/** Put the datagram whose header is in `hdr` and whose `plen` bytes of
 * payload are at `payload` on the loopback ring, counting a refusal in
 * `net_mib`. */
static int loop_put(const uint8_t *hdr, const uint8_t *payload, uint16_t plen)
{
    int r = ip_loop_put_parts(hdr, IP_HDR_SIZE, payload, plen, 0);

    if (r != 0) {
        net_mib.ip_out_discards++;
    }
    return r;
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
    return ip_tx_opts(0, dst_ip, proto, payload, plen, ttl, IP_PMTUDISC_WANT);
}

int ip_tx_from(uint32_t src, uint32_t dst_ip, uint8_t proto, const uint8_t *payload, uint16_t plen,
               uint8_t ttl)
{
    return ip_tx_opts(src, dst_ip, proto, payload, plen, ttl, IP_PMTUDISC_WANT);
}

/** Write a 20-byte header into `buf`: a datagram of `total_len` bytes with
 * the identification `id`, the flags and offset field `frag`, the hop limit
 * `ttl` and protocol `proto`, from `src` to `dst`, and its checksum. */
static void ip_header(uint8_t *buf, uint16_t total_len, uint16_t id, uint16_t frag, uint8_t ttl,
                      uint8_t proto, uint32_t src, uint32_t dst)
{
    buf[0] = 0x45; /* version 4, IHL 5 */
    buf[1] = 0x00; /* DSCP/ECN */
    write16be(buf + 2, total_len);
    write16be(buf + 4, id);
    write16be(buf + 6, frag);
    buf[8] = ttl;
    buf[9] = proto;
    write16be(buf + 10, 0x0000);
    write32be(buf + 12, src);
    write32be(buf + 16, dst);
    write16be(buf + 10, ip_checksum(buf, IP_HDR_SIZE));
}

/**
 * Hand the datagram of `len` bytes in `ip_tx_buf` to `out`, toward
 * `route_ip`: to the all-ones station address for a broadcast to `dst_ip`,
 * else to the next hop's from ARP. On a miss the datagram is held and an
 * ARP request goes out. Returns 0, -EMSGSIZE or -ENOBUFS, as `ip_tx_opts`
 * says.
 */
static int ip_emit(struct netdev *out, uint32_t route_ip, uint32_t dst_ip, uint16_t len)
{
    static const uint8_t all_ones[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    uint8_t dst_mac[6];
    const uint8_t *to = dst_mac;
    int sent;

    if (netconf_is_broadcast(dst_ip)) {
        to = all_ones;
    } else if (!arp_lookup(out, route_ip, dst_mac)) {
        ip_hold_put(out, route_ip, ip_tx_buf, ip_tx_buf + IP_HDR_SIZE,
                    (uint16_t)(len - IP_HDR_SIZE), 0);
        arp_request(out, route_ip);
        return 0;
    }
    sent = ethernet_tx(out, to, ETH_TYPE_IP, ip_tx_buf, len);
    if (sent == ETHERNET_TX_TOOLONG) {
        return -EMSGSIZE;
    }
    if (sent != NETDEV_TX_OK) {
        net_mib.ip_out_discards++;
        return -ENOBUFS;
    }
    return 0;
}

/**
 * Send the `plen` bytes at `payload` by `out` to the station address `mac`
 * in fragments of at most `mtu` bytes each, built in `buf`: the most payload
 * `mtu` leaves room for, a multiple of 8, 1480 bytes at 1500, each with MF
 * but the last. Each fragment's header is `hdr`'s, the 20 bytes of a
 * header without options, with its own length and offset. The first
 * fragment that cannot leave ends the send, counted in FragFails. Returns
 * 0, -EMSGSIZE or -ENOBUFS, as `ip_tx_opts` says.
 */
static int ip_fragment_to(struct netdev *out, const uint8_t *mac, const uint8_t *hdr,
                          const uint8_t *payload, uint16_t plen, uint32_t mtu, uint8_t *buf)
{
    uint32_t unit = (mtu - IP_HDR_SIZE) & ~7u;
    uint32_t off;
    int sent;

    for (off = 0; off < plen; off += unit) {
        uint32_t n = plen - off < unit ? plen - off : unit;
        uint16_t frag = (uint16_t)(off >> 3);

        if (off + n < plen) {
            frag |= IP_MF;
        }
        ip_header(buf, (uint16_t)(IP_HDR_SIZE + n), read16be(hdr + 4), frag, hdr[8], hdr[9],
                  read32be(hdr + 12), read32be(hdr + 16));
        k_memcpy(buf + IP_HDR_SIZE, payload + off, n);
        sent = ethernet_tx(out, mac, ETH_TYPE_IP, buf, (uint16_t)(IP_HDR_SIZE + n));
        if (sent != NETDEV_TX_OK) {
            net_mib.ip_frag_fails++;
            if (sent == ETHERNET_TX_TOOLONG) {
                return -EMSGSIZE;
            }
            net_mib.ip_out_discards++;
            return -ENOBUFS;
        }
        net_mib.ip_frag_creates++;
    }
    net_mib.ip_frag_oks++;
    return 0;
}

/**
 * Send the `plen` bytes at `payload` in fragments of at most `mtu` bytes
 * each by `out`, as `ip_tx_opts` says, to the all-ones station address for
 * a broadcast, else to the next hop's from ARP. On a miss the datagram is
 * held whole and an ARP request goes out.
 */
static int ip_fragment(struct netdev *out, uint32_t route_ip, uint32_t src, uint32_t dst_ip,
                       uint8_t proto, const uint8_t *payload, uint16_t plen, uint8_t ttl,
                       uint32_t mtu)
{
    static const uint8_t all_ones[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    uint8_t hdr[IP_HDR_SIZE], mac[6];

    ip_header(hdr, (uint16_t)(IP_HDR_SIZE + plen), ip_id_counter++, 0, ttl, proto, src, dst_ip);
    if (netconf_is_broadcast(dst_ip)) {
        return ip_fragment_to(out, all_ones, hdr, payload, plen, mtu, ip_tx_buf);
    }
    if (!arp_lookup(out, route_ip, mac)) {
        ip_hold_put(out, route_ip, hdr, payload, plen, mtu);
        arp_request(out, route_ip);
        return 0;
    }
    return ip_fragment_to(out, mac, hdr, payload, plen, mtu, ip_tx_buf);
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
int ip_tx_opts(uint32_t src, uint32_t dst_ip, uint8_t proto, const uint8_t *payload, uint16_t plen,
               uint8_t ttl, int pmtudisc)
{
    uint32_t total_len = (uint32_t)IP_HDR_SIZE + plen;
    uint32_t route_ip, mtu;
    uint16_t df;
    struct netdev *out;
    int local;

    if (plen > IP_PAYLOAD_MAX) {
        klog(KLOG_WARN, "ip", "tx: payload of %d bytes is too large, refused", (int)plen);
        return -EMSGSIZE;
    }

    net_mib.ip_out_requests++;

    if (ip_ns && (!IP_IS_LOOPBACK(dst_ip) || !ip_ns->lo || !ip_ns->lo->up || !ip_ns->lo->ip)) {
        net_mib.ip_out_no_routes++;
        return -ENETUNREACH;
    }

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
    if (src == 0 || !ip_is_local(src)) {
        src = IP_IS_LOOPBACK(dst_ip) ? IP_LOOPBACK_ADDR
              : out                  ? netdev_source(out, route_ip)
                                     : net_ip_addr;
    }
    local = IP_IS_LOOPBACK(dst_ip) || netdev_owns(dst_ip);
    if (!local && netconf_is_broadcast(dst_ip) && proto != IP_PROTO_UDP) {
        klog(KLOG_WARN, "ip", "tx: refused to broadcast a datagram that is not UDP");
        return -EOPNOTSUPP;
    }

    if (local) {
        mtu = NETDEV_LOOPBACK_MTU;
    } else if (pmtudisc == IP_PMTUDISC_INTERFACE || pmtudisc == IP_PMTUDISC_OMIT) {
        mtu = out->mtu;
    } else {
        mtu = ip_path_mtu(out, dst_ip);
    }
    if (total_len > mtu) {
        if (pmtudisc == IP_PMTUDISC_DO || pmtudisc == IP_PMTUDISC_PROBE) {
            net_mib.ip_frag_fails++;
            return -EMSGSIZE;
        }
        return ip_fragment(out, route_ip, src, dst_ip, proto, payload, plen, ttl, mtu);
    }

    df = pmtudisc == IP_PMTUDISC_DONT || pmtudisc == IP_PMTUDISC_OMIT ||
                 pmtudisc == IP_PMTUDISC_INTERFACE
             ? 0
             : IP_DF;
    ip_header(ip_tx_buf, (uint16_t)total_len, ip_id_counter++, df, ttl, proto, src, dst_ip);
    if (local) {
        return loop_put(ip_tx_buf, payload, plen);
    }
    k_memcpy(ip_tx_buf + IP_HDR_SIZE, payload, plen);
    return ip_emit(out, route_ip, dst_ip, (uint16_t)total_len);
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

/* Datagrams a tick found on the ring with every processor idle. */
static uint32_t loop_left_idle;

void ip_loop_count_idle(void)
{
    loop_left_idle += loop_head - loop_tail;
}

uint32_t ip_loop_left_idle(void)
{
    return loop_left_idle;
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

        const uint8_t *dg = loop_ring_big[slot] ? loop_ring_big[slot] : loop_ring[slot];
        struct netdev *lo;

        ip_ns = loop_ring_ns[slot];
        lo = loop_ring_len[slot] == 0 ? 0 : ip_ns ? ip_ns->lo : netdev_loopback();
        if (lo) {
            lo->rx_packets++;
            lo->rx_bytes += loop_ring_len[slot];
        }
        if (loop_ring_len[slot] == 0) {
            /* dropped when its namespace ended */
        } else if ((dg[0] >> 4) == 6) {
            ip6_loop_rx(loop_ring_if[slot], dg, loop_ring_len[slot]);
        } else if (loop_ring_len[slot] <= 0xFFFFu) {
            ip_rx(dg, (uint16_t)loop_ring_len[slot], 1);
        }
        ip_ns = 0;
        kfree(loop_ring_big[slot]);
        loop_ring_big[slot] = 0;
        loop_tail++;
        if (from >= 0 && from < 3) {
            loop_delivered[from]++;
        }
    }
    loop_draining = 0;
}

void ip_loop_ns_gone(const struct netns *ns)
{
    uint32_t at;

    for (at = loop_tail; at != loop_head; at++) {
        if (loop_ring_ns[at % LOOP_RING_SLOTS] == ns) {
            loop_ring_len[at % LOOP_RING_SLOTS] = 0;
        }
    }
}

void ip_loop_drain(void)
{
    ip_loop_drain_from(IP_LOOP_FROM_TICK);
}
