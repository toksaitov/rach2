/*
 * tcp.c: TCP, as tcp.h describes it: the control blocks, the state machine,
 * and the segments sent and received.
 *
 * A passive open: `tcp_listen` makes a LISTEN block; a SYN to it makes a
 * block and a socket for the connection and is answered with a SYN-ACK; the
 * ACK completes the handshake and queues the socket for `accept`. An active
 * open: `tcp_connect` sends a SYN and enters SYN_SENT, and the SYN-ACK
 * completes the handshake.
 *
 * Reliability: RFC 6298's round-trip estimate and timeout, with Karn's
 * algorithm; after a timeout, go-back-N retransmission paced by the
 * congestion window; fast retransmit and NewReno recovery from the third
 * duplicate acknowledgement (RFC 5681, RFC 6582); RFC 5681's slow start and
 * congestion avoidance; zero-window probes; path MTU black-hole detection
 * (RFC 4821); and TIME_WAIT. A SYN, SYN-ACK or FIN is sent again too, and a
 * connection that runs out of retransmissions, or whose keepalive probes go
 * unanswered, is closed with ETIMEDOUT, so that a blocked `connect`, `send`
 * or `recv` returns an error.
 *
 * A segment received has its checksum, over the pseudo-header and the
 * segment, checked. A reset closes a connection only at the next expected
 * sequence number, and one elsewhere in the receive window is answered with
 * an acknowledgement (RFC 5961). An initial sequence number is a clock plus
 * a keyed hash of the connection's addresses and ports (RFC 6528).
 */

#include "../../idle_timer.h"
#include "../../klog.h"
#include "../../lib/lockdep.h"
#include "../../lib/kerrno.h"
#include "tcp.h"
#include "rcvwnd.h"
#include "../socket/socket.h"
#include "../ip/ip.h"
#include "../ip6/ip6.h"
#include "../ip6/nd6.h"
#include "../core/byteorder.h"
#include "../net.h"
#include "../netconf.h"
#include "../driver/netdev.h"
#include "../../serial.h"
#include "../../lib/kstring.h"
#include "../../lib/kerrno.h"
#include "../../lib/krandom.h"
#include "../../mm/kheap.h"
#include "../../mm/pressure.h"
#include "../../mm/paging.h"
#include "../../timer.h"

/*
 * The control blocks, listeners included, each allocated from the heap by
 * `listen`, by an active open, or for an incoming SYN on the interrupt side,
 * and freed when it is done; its rings are allocations of their own. The
 * list holds the blocks that run timers first, and then, from `tw_first` to
 * `tcb_last`, the detached blocks in TIME_WAIT, `tw_count` of them, in the
 * order their TIME_WAIT ends. A burst of short connections leaves thousands
 * there for TCP_2MSL_TICKS, and the tick, the listener's search and the
 * pumps stop at `tw_first`, so that they pass over none; `tw_owed` is set
 * while one of them owes an acknowledgement the card refused.
 */
static struct tcb *tcbs, *tcb_last, *tw_first;
static uint32_t tw_count;
static int tw_owed;

/* The most blocks the TIME_WAIT run holds, as Linux's tcp_max_tw_buckets:
 * half the buckets of its table of connections, a power of two near one for
 * each 128 KiB of memory; 0 until `tcp_tw_max` first computes it. */
static uint32_t tw_max;

/* The table of connections `tcp_find` reads: every block but a listener,
 * chained by `hash_next` in the bucket of its local port and remote address
 * and port (`tcp_hash_index`), as Linux's established hash. */
#define TCP_HASH_SIZE 4096
static struct tcb *tcp_hash[TCP_HASH_SIZE];

/* The ports in use, indexed by port, as Linux's table of bound ports: the
 * blocks and bound stream sockets that hold a port no other connection may
 * share (`port_fixed`), and the connections whose port `tcp_connect` chose,
 * which one to another peer may share (`port_auto`). Every port is
 * counted, since a write of ip_local_port_range may move the ephemeral
 * range over ports bound before it. */
static uint32_t port_fixed[65536];
static uint32_t port_auto[65536];

/* Where the next connection to a destination begins its search for a port,
 * by a keyed hash of the destination, as Linux's table_perturb (RFC 6056,
 * algorithm 4), and the searches of `tcp_free_port` begun, which key
 * where each starts. */
#define TCP_PERTURB_SIZE 256
static uint32_t port_perturb[TCP_PERTURB_SIZE];
static uint32_t bind_searches;

int tcp_keepalive_time = TCP_KEEPIDLE_DEFAULT;
int tcp_keepalive_intvl = TCP_KEEPINTVL_DEFAULT;
int tcp_keepalive_probes = TCP_KEEPCNT_DEFAULT;
int tcp_retries2 = TCP_RETRIES2_DEFAULT;
int tcp_mtu_probing = TCP_MTU_PROBING_DEFAULT;
int tcp_delack = 1;
int tcp_ack_batch = 1;

/* Linux's TCP_DELACK_MIN, HZ / 25, 40 ms: the longest an acknowledgement
 * is held, and the time within which data sent after data received counts
 * as an answer (`pingpong`). */
#define TCP_DELACK_TICKS  (TICK_HZ / 25u)
/* Linux's TCP_MIN_MSS: a segment shorter than this, header included less
 * 20, ends a write of the peer's even without PSH (tcp_measure_rcv_mss). */
#define TCP_DELACK_TINY   88u
/* Linux's TCP_MAX_QUICKACKS: the most acknowledgements a grant sends at
 * once (`tcp_delack_quick`). */
#define TCP_DELACK_QUICK  16u
/* The most data one batched acknowledgement covers, as Linux's GRO merges
 * at most 64 KiB into the one segment TCP acknowledges. */
#define TCP_BATCH_ACK_MAX 65536u

/* The batches open (`tcp_batch_begin`), and the blocks that owe an
 * acknowledgement at the outermost one's end. */
static int tcp_batch_depth;
static struct tcb *tcp_batch_list;

static void tcp_delack_quick(struct tcb *tcb);
static void tcp_enter_quickack(struct tcb *tcb);

/** Drop the acknowledgement work of `tcb`, which a reset or a give-up
 * closed: the held one, its marks and one the driver refused. */
static void tcp_delack_clear(struct tcb *tcb)
{
    tcb->delack_due = 0;
    tcb->ack_pushed = 0;
    tcb->ack_owed = 0;
}

/* The segment being sent: a header with one word of options and TCP_MSS
 * bytes of data, or a header alone with more options. */
static uint8_t tcp_tx_buf[TCP_HDR_SIZE + 4 + TCP_MSS];

/* The 12-byte pseudo-header and a copy of the segment, which the checksum
 * is computed over. */
static uint8_t tcp_cksum_buf[12 + TCP_HDR_SIZE + 4 + TCP_MSS];

/* The largest segment taken in, the most `tcp_cksum_buf` has room for. */
#define TCP_SEG_MAX (TCP_HDR_SIZE + 4 + TCP_MSS)

/* The option kinds a SYN's options are read for (RFC 9293). */
#define TCP_OPT_END     0
#define TCP_OPT_NOP     1
#define TCP_OPT_MSS     2
#define TCP_OPT_WS      3
#define TCP_OPT_SACK_OK 4
#define TCP_OPT_SACK    5

static uint16_t tcp_checksum(net_addr_t src_ip, net_addr_t dst_ip, const uint8_t *seg,
                             uint16_t seg_len);
static int tcp_emit_flags(struct tcb *tcb, uint8_t flags, uint32_t seq);
static void tcp_send_flags(struct tcb *tcb, uint8_t flags);
static int tcp_output(struct tcb *tcb);
static int tcp_resend(struct tcb *tcb, uint32_t seq, uint32_t most);
static int tcp_recover_send(struct tcb *tcb);
static void tcp_ack_grow(struct tcb *tcb);
static void tcp_pump(struct tcb *tcb);
static void tcp_giveup(struct tcb *tcb);
static struct tcb *tcp_find(net_addr_t local_ip, uint16_t local_port, net_addr_t remote_ip,
                            uint16_t remote_port);
static void tcp_free_half_opens(const struct tcb *listener);
static int tcp_pair_in_use(const struct netns *ns, net_addr_t local_ip, uint16_t local_port,
                           net_addr_t remote_ip, uint16_t remote_port, int ifindex);
static void tcp_reset(struct tcb *tcb);
static int tcp_oow_limited(struct tcb *tcb, uint32_t *skipped);

/* The key the initial sequence numbers are hashed under, and whether it has
 * been drawn; `tcp_isn_key` draws it. */
static uint32_t isn_key[4];
static int isn_keyed;

/* RFC 6528's timer counts 4-microsecond units, 250000 a second. */
#define TCP_ISN_HZ 250000u

/* The low word of the tick count, which TCP's 32-bit stamps and deadlines
 * are differences against, each shorter than the word's cycle. */
#define TCP_NOW() ((uint32_t)kernel_ticks)

/**
 * The deadline `ticks` from now of a retransmission or zero-window timer,
 * which `tcp_tick` compares with the tick count, as Linux's timers keep
 * `jiffies + when`: the boot processor's tick does not call it on every
 * tick, and a countdown of its calls would run slow. It is never 0, which
 * means off; a deadline that falls on tick 0 is kept as tick 1.
 */
static uint32_t tcp_due(uint32_t ticks)
{
    uint32_t due = TCP_NOW() + ticks;

    return due != 0 ? due : 1u;
}

/** Whether the deadline `due` of a running timer has come. */
static int tcp_expired(uint32_t due)
{
    return (int32_t)(TCP_NOW() - due) >= 0;
}

/** The ticks left until the deadline `due` of a running timer, at least 1,
 * or 0 when `due` is 0 and the timer is off. */
static uint32_t tcp_left(uint32_t due)
{
    int32_t left = (int32_t)(due - TCP_NOW());

    return due == 0 ? 0u : left > 0 ? (uint32_t)left : 1u;
}

#define TCP_U64(hi, lo)  (((uint64_t)(hi) << 32) | (uint64_t)(lo))
#define TCP_ROTL64(x, b) (((x) << (b)) | ((x) >> (64 - (b))))

/* One SipHash round over its four state words. */
#define TCP_SIPROUND(v0, v1, v2, v3) \
    do {                             \
        (v0) += (v1);                \
        (v1) = TCP_ROTL64((v1), 13); \
        (v1) ^= (v0);                \
        (v0) = TCP_ROTL64((v0), 32); \
        (v2) += (v3);                \
        (v3) = TCP_ROTL64((v3), 16); \
        (v3) ^= (v2);                \
        (v0) += (v3);                \
        (v3) = TCP_ROTL64((v3), 21); \
        (v3) ^= (v0);                \
        (v2) += (v1);                \
        (v1) = TCP_ROTL64((v1), 17); \
        (v1) ^= (v2);                \
        (v2) = TCP_ROTL64((v2), 32); \
    } while (0)

/**
 * Draw the key for the initial sequence numbers from `krandom`, once.
 *
 * Called in system call context by `tcp_listen` and `tcp_connect`, since
 * `krandom` may not be called from an interrupt and a passive open chooses
 * its number in one; a passive open needs a listener, so the key is drawn
 * before its SYN can arrive. The caller holds `net_lock`.
 */
static void tcp_isn_key(void)
{
    int i;

    if (isn_keyed) {
        return;
    }
    for (i = 0; i < 4; i++) {
        isn_key[i] = krandom_u32();
    }
    isn_keyed = 1;
}

/**
 * SipHash-2-4 under `isn_key` of the local and remote addresses and ports,
 * `local`'s family deciding the form; IPv6 includes both scopes. Returns the
 * low 32 bits.
 */
static uint32_t tcp_tuple_hash(const net_addr_t *local, const net_addr_t *remote,
                               uint16_t local_port, uint16_t remote_port)
{
    uint64_t k0 = TCP_U64(isn_key[1], isn_key[0]);
    uint64_t k1 = TCP_U64(isn_key[3], isn_key[2]);
    uint64_t v0 = k0 ^ TCP_U64(0x736f6d65u, 0x70736575u);
    uint64_t v1 = k1 ^ TCP_U64(0x646f7261u, 0x6e646f6du);
    uint64_t v2 = k0 ^ TCP_U64(0x6c796765u, 0x6e657261u);
    uint64_t v3 = k1 ^ TCP_U64(0x74656462u, 0x79746573u);
    uint64_t m = TCP_U64(remote->v4, local->v4);
    uint64_t last = TCP_U64(12u << 24, ((uint32_t)remote_port << 16) | local_port);
    int i;

    if (local->family == AF_INET6) {
        uint8_t tuple[40];
        k_memcpy(tuple, local->v6, 16);
        k_memcpy(tuple + 16, remote->v6, 16);
        write32be(tuple + 32, local->scope);
        write32be(tuple + 36, remote->scope);
        for (i = 0; i < 40; i += 8) {
            m = TCP_U64(read32be(tuple + i), read32be(tuple + i + 4));
            v3 ^= m;
            TCP_SIPROUND(v0, v1, v2, v3);
            TCP_SIPROUND(v0, v1, v2, v3);
            v0 ^= m;
        }
        m = TCP_U64(44u << 24, ((uint32_t)remote_port << 16) | local_port);
        last = m;
    }

    if (local->family != AF_INET6) {
        v3 ^= m;
        TCP_SIPROUND(v0, v1, v2, v3);
        TCP_SIPROUND(v0, v1, v2, v3);
        v0 ^= m;
    }
    v3 ^= last;
    TCP_SIPROUND(v0, v1, v2, v3);
    TCP_SIPROUND(v0, v1, v2, v3);
    v0 ^= last;
    v2 ^= 0xffu;
    TCP_SIPROUND(v0, v1, v2, v3);
    TCP_SIPROUND(v0, v1, v2, v3);
    TCP_SIPROUND(v0, v1, v2, v3);
    TCP_SIPROUND(v0, v1, v2, v3);
    return (uint32_t)(v0 ^ v1 ^ v2 ^ v3);
}

/**
 * The initial sequence number for `tcb`, whose addresses and ports are set,
 * in the form RFC 6528 gives: a 4-microsecond timer plus a keyed hash of the
 * connection's two addresses and two ports.
 *
 * The timer is the tick count in 4-microsecond units, so a connection opened
 * again on the same addresses and ports starts above where the last one
 * began. The hash is what a host off the path cannot compute, so that the
 * time and the ports do not give the number away.
 */
static uint32_t tcp_isn(const struct tcb *tcb)
{
    return (uint32_t)tick_div64((uint64_t)TCP_NOW() * TCP_ISN_HZ, TICK_HZ, NULL) +
           tcp_tuple_hash(&tcb->local_ip, &tcb->remote_ip, tcb->local_port, tcb->remote_port);
}

void tcp_init(void)
{
    tcbs = tcb_last = tw_first = NULL;
    tw_count = 0;
    tw_owed = 0;
    k_memset(tcp_hash, 0, sizeof(tcp_hash));
    k_memset(port_fixed, 0, sizeof(port_fixed));
    k_memset(port_auto, 0, sizeof(port_auto));
    k_memset(port_perturb, 0, sizeof(port_perturb));
    isn_keyed = 0;
}

/**
 * The window to offer, in bytes: the receive ring's free space, clamped to
 * what the 16-bit field carries at the connection's scale. An unscaled
 * ring of 64 KiB is one larger than the field, so a cast alone would offer
 * zero for an empty ring.
 *
 * Under `tcp_delack` it is Linux's __tcp_select_window and tcp_select_window
 * besides: while less than half the largest window is free, less than a
 * sixteenth of it or than `rcv_mss` is offered as zero, and never less than
 * what the peer was offered already (`rcv_adv`), which the ring always has
 * room for, since data that came in has taken only space offered before.
 */
static uint32_t tcp_rcv_window(const struct tcb *tcb)
{
    uint32_t free_space = tcb->recv_size - (tcb->recv_tail - tcb->recv_head);
    uint32_t most = (uint32_t)TCP_WINDOW_SIZE << tcb->rcv_wscale;
    uint32_t wnd = free_space > most ? most : free_space;
    uint32_t clamp = most > tcb->recv_size ? tcb->recv_size : most, left;

    if (!tcp_delack || !tcb->rcv_adv_set) {
        return wnd;
    }
    if (wnd < clamp / 2u && (wnd < clamp / 16u || wnd < tcb->rcv_mss)) {
        wnd = 0;
    }
    left = tcb->rcv_adv - tcb->rcv_nxt;
    if ((int32_t)left > 0 && wnd < left) {
        wnd = left;
    }
    return wnd;
}

/**
 * The window field of a segment of `flags` about to be sent: the window
 * shifted by the connection's scale, except in a SYN, whose field is never
 * scaled (RFC 7323). The right edge the field offers moves `rcv_adv`
 * forward, never back, and `rcv_wup` to `rcv_nxt`, in a segment that
 * carries an ACK, since only those follow the peer's SYN, which `rcv_nxt`
 * is taken from: the SYN of an active open records nothing, as Linux takes
 * rcv_wup from the peer's SYN.
 *
 * Under `tcp_delack` a segment with ACK and without SYN before TIME_WAIT
 * offers the window selected now (`tcp_rcv_window`), data and probes
 * included, as Linux's tcp_transmit_skb selects it for each, and ends
 * quick mode while less than half the largest window is free, as Linux's
 * __tcp_select_window does. A SYN-ACK offers the stored `rcv_wnd`. In
 * TIME_WAIT, which takes no more data and may have freed its ring, a
 * segment offers the saved
 * `rcv_wnd`, selected on entry or before it, even after a read and when
 * the first acknowledgement of the FIN was refused, as Linux's tw_rcv_wnd
 * keeps it.
 */
static uint16_t tcp_wnd_field(struct tcb *tcb, uint8_t flags)
{
    int syn = (flags & TCP_SYN) != 0;
    uint32_t field, edge, most = (uint32_t)TCP_WINDOW_SIZE << tcb->rcv_wscale;

    if (tcp_delack && !syn && (flags & TCP_ACK) && tcb->state != TCP_TIME_WAIT) {
        tcb->rcv_wnd = tcp_rcv_window(tcb);
        if (tcb->recv_size - (tcb->recv_tail - tcb->recv_head) <
            (most > tcb->recv_size ? tcb->recv_size : most) / 2u) {
            tcb->ack_quick = 0;
        }
    }
    field = syn ? tcb->rcv_wnd : tcb->rcv_wnd >> tcb->rcv_wscale;

    if (field > TCP_WINDOW_SIZE) {
        field = TCP_WINDOW_SIZE;
    }
    edge = tcb->rcv_nxt + (syn ? field : field << tcb->rcv_wscale);
    if ((flags & TCP_ACK) && (!tcb->rcv_adv_set || (int32_t)(edge - tcb->rcv_adv) > 0)) {
        tcb->rcv_adv = edge;
        tcb->rcv_adv_set = 1;
    }
    if (flags & TCP_ACK) {
        tcb->rcv_wup = tcb->rcv_nxt;
    }
    return (uint16_t)field;
}

/**
 * Give a connection its rings before its first SYN: the send ring, of
 * TCP_RING_SIZE bytes, and the receive ring, TCP_RECV_RING_LARGE when more
 * than TCP_BIG_RING_FRAMES frames are free, TCP_RECV_RING_SMALL otherwise or
 * when the heap has no room for the large one, and the window scale the
 * receive ring needs, which this side's SYN offers. Returns 0, or -1 when the
 * heap has no room for the send ring and a receive ring; `tcb_free` gives
 * back what was had.
 */
static int tcp_ring_alloc(struct tcb *tcb)
{
    uint32_t size =
        frame_free_count() > TCP_BIG_RING_FRAMES ? TCP_RECV_RING_LARGE : TCP_RECV_RING_SMALL;
    uint8_t shift = 0;

    tcb->send_buf = kmalloc(TCP_RING_SIZE);
    if (!tcb->send_buf) {
        return -1;
    }
    tcb->recv_buf = kmalloc(size);
    if (!tcb->recv_buf && size != TCP_RECV_RING_SMALL) {
        size = TCP_RECV_RING_SMALL;
        tcb->recv_buf = kmalloc(size);
    }
    if (!tcb->recv_buf) {
        return -1;
    }
    while ((size >> shift) > TCP_WINDOW_SIZE) {
        shift++;
    }
    tcb->recv_size = size;
    tcb->rcv_wscale = shift;
    tcb->ws_offer = shift != 0;
    tcb->rcv_wnd = tcp_rcv_window(tcb);
    return 0;
}

/** Give a new, zeroed block the starting values of a connection: CLOSED,
 * the first window, threshold, congestion window, segment size and timeout,
 * and a SACK offer. */
static void tcb_init(struct tcb *tcb)
{
    tcb->in_use = 1;
    tcb->state = TCP_CLOSED;
    tcb->rcv_wnd = TCP_WINDOW_SIZE;
    tcb->ssthresh = 65535;
    tcb->cwnd = TCP_INIT_CWND;
    tcb->snd_mss = TCP_MSS_DEFAULT;
    tcb->rcv_mss = TCP_MSS_DEFAULT;
    tcb->rto = TCP_RTO_INIT;
    tcb->sack_offer = 1;
}

/**
 * The MTUs of the path `tcb` takes, IP header included: the path's, which a
 * fragmentation-needed report or a Packet Too Big may have lowered below
 * the interface's (`ip_path_mtu`, `ip6_path_mtu`), and in `*link` the
 * interface's, for IPv6 the link's (`nd6_link_mtu`). A datagram to this
 * machine goes round the loopback backlog, whose MTU is `lo`'s. With no route
 * an IPv6 path counts as the least MTU, which keeps a retransmission small
 * enough for any path, and an IPv4 path as a card's largest, 1500.
 */
static uint32_t tcp_mtus(const struct tcb *tcb, uint32_t *link)
{
    uint32_t dst = tcb->remote_ip.v4, via;
    uint8_t via6[16];
    struct netdev *nd;

    if (tcb->remote_ip.family == AF_INET6) {
        nd = ip6_route_find(tcb->remote_ip.v6,
                            tcb->ifindex ? tcb->ifindex : (int)tcb->remote_ip.scope, via6);
        if (!nd) {
            *link = IP6_MTU_MIN;
            return IP6_MTU_MIN;
        }
        *link = nd->loopback ? (uint32_t)NETDEV_LOOPBACK_MTU : nd6_link_mtu(nd);
        return ip6_path_mtu(nd, tcb->remote_ip.v6);
    }
    if (IP_IS_LOOPBACK(dst) || netdev_owns(dst)) {
        *link = NETDEV_LOOPBACK_MTU;
        return NETDEV_LOOPBACK_MTU;
    }
    nd = netdev_route_on(dst, tcb->ifindex, &via);
    if (!nd) {
        *link = NETDEV_ETH_MTU_MAX;
        return NETDEV_ETH_MTU_MAX;
    }
    *link = nd->mtu;
    return ip_path_mtu(nd, dst);
}

/** The segment size an MTU of `mtu` bytes leaves room for on `tcb`'s
 * family, past 40 bytes of IPv4 and TCP headers or 60 of IPv6 and TCP: at
 * most TCP_MSS, which this file's buffers hold, and at least TCP_MSS_MIN. */
static uint16_t tcp_mtu_to_mss(const struct tcb *tcb, uint32_t mtu)
{
    uint32_t hdrs = (tcb->remote_ip.family == AF_INET6 ? IP6_HDR_SIZE : IP_HDR_SIZE) + TCP_HDR_SIZE;

    if (mtu > (uint32_t)TCP_MSS + hdrs) {
        return TCP_MSS;
    }
    if (mtu < (uint32_t)TCP_MSS_MIN + hdrs) {
        return TCP_MSS_MIN;
    }
    return (uint16_t)(mtu - hdrs);
}

/** The largest segment `tcb` may send on its path now, as `tcp_mtus` gives
 * the path's MTU. */
static uint16_t tcp_payload_limit(const struct tcb *tcb)
{
    uint32_t link;

    return tcp_mtu_to_mss(tcb, tcp_mtus(tcb, &link));
}

/** The MSS a SYN of `tcb` offers: the interface's MTU less the headers, as
 * Linux offers it, whatever a report has lowered the path to, since the
 * offer says what this end of the path receives. */
static uint16_t tcp_offer_mss(const struct tcb *tcb)
{
    uint32_t link;

    tcp_mtus(tcb, &link);
    return tcp_mtu_to_mss(tcb, link);
}

/** The least segment size black-hole detection lowers a connection's to:
 * TCP_MSS_DEFAULT on IPv4, TCP_MSS_FLOOR6, IPv6's least MTU less its
 * headers, on IPv6. */
static uint16_t tcp_mss_floor(const struct tcb *tcb)
{
    return (uint16_t)(tcb->remote_ip.family == AF_INET6 ? TCP_MSS_FLOOR6 : TCP_MSS_DEFAULT);
}

/** Keep the segment size and the bounds of the search for a larger one
 * within what the path carries now (`tcp_payload_limit`). */
static void tcp_mss_clamp(struct tcb *tcb)
{
    uint16_t most = tcp_payload_limit(tcb);

    if (tcb->snd_mss > most)
        tcb->snd_mss = most;
    if (tcb->mss_max > most)
        tcb->mss_max = most;
    if (tcb->mss_hi > most)
        tcb->mss_hi = most;
}

/** `ms` milliseconds in ticks, rounded up as TCP_MS rounds, for a count of
 * milliseconds too large for TCP_MS's product. */
static uint32_t tcp_ms_ticks(uint32_t ms)
{
    return MS_TO_TICKS(ms);
}

/**
 * Take the peer's options from its SYN, `data` with a header of `hdr_len`
 * bytes.
 *
 * The segment size is the value of its MSS option, at most TCP_MSS and at
 * least TCP_MSS_MIN, or TCP_MSS_DEFAULT when it has none; every segment
 * this side sends is at most that size, and the congestion window starts
 * at TCP_INIT_SEGS of them. The peer's value is what its end of the path
 * carries, and a router on the way may have lowered it to fit a smaller
 * link, as a PPPoE line's router does. The segments are sent with DF set,
 * so one larger than the path carries is dropped each time it is sent.
 *
 * Windows are scaled when both SYNs have the window scale option: the
 * peer's shift, at most 14, applies to the window fields it sends, and
 * this side's, which its SYN offered, to the fields it sends. When either
 * SYN lacks the option neither side scales, and this side offers at most
 * 65535 bytes whatever its ring holds.
 *
 * Selective acknowledgements are sent when both SYNs permit them; this
 * side's SYN offers them always, and its answer to a SYN only when that
 * SYN did.
 *
 * The segment size is also the largest the search for a larger one may
 * return to (`mss_max`); with `tcp_mtu_probing` 2 or more, as Linux's
 * `tcp_mtup_init` reads it, the connection starts at
 * TCP_BASE_MSS, or the floor when that is larger, and probes upward.
 */
static void tcp_take_options(struct tcb *tcb, const uint8_t *data, uint16_t hdr_len)
{
    uint16_t i = TCP_HDR_SIZE;
    uint16_t mss = tcb->remote_ip.family == AF_INET6 ? 1220 : TCP_MSS_DEFAULT;
    int scaled = 0;
    int sack = 0;
    uint8_t shift = 0;

    while (i < hdr_len && data[i] != TCP_OPT_END) {
        if (data[i] == TCP_OPT_NOP) {
            i++;
            continue;
        }
        if (i + 1u >= hdr_len || data[i + 1] < 2 || i + data[i + 1] > hdr_len)
            break;
        if (data[i] == TCP_OPT_MSS && data[i + 1] == 4) {
            mss = read16be(data + i + 2);
            if (mss > TCP_MSS)
                mss = TCP_MSS;
            if (mss < TCP_MSS_MIN)
                mss = TCP_MSS_MIN;
        } else if (data[i] == TCP_OPT_WS && data[i + 1] == 3) {
            scaled = 1;
            shift = data[i + 2] > 14 ? 14 : data[i + 2];
        } else if (data[i] == TCP_OPT_SACK_OK && data[i + 1] == 2) {
            sack = 1;
        }
        i = (uint16_t)(i + data[i + 1]);
    }
    tcb->sack_ok = (uint8_t)(sack && tcb->sack_offer);
    tcb->sack_offer = tcb->sack_ok;
    if (mss > tcp_payload_limit(tcb))
        mss = tcp_payload_limit(tcb);
    tcb->snd_mss = mss;
    tcb->mss_max = mss;
    tcb->mss_hi = mss;
    if (tcp_mtu_probing >= 2) {
        uint16_t base = TCP_BASE_MSS < tcp_mss_floor(tcb) ? tcp_mss_floor(tcb) : TCP_BASE_MSS;

        tcb->mtup_on = 1;
        tcb->probe_after = TCP_NOW();
        if (mss > base)
            tcb->snd_mss = base;
    }
    tcb->cwnd = (uint32_t)TCP_INIT_SEGS * tcb->snd_mss;
    if (scaled && tcb->ws_offer) {
        tcb->snd_wscale = shift;
    } else {
        tcb->snd_wscale = 0;
        tcb->rcv_wscale = 0;
        tcb->ws_offer = 0;
    }
    tcb->rcv_wnd = tcp_rcv_window(tcb);
}

/**
 * The bucket of `tcp_hash` for a connection with this local port and remote
 * address and port. The local address is left out, so that a port's
 * connections to one peer share a bucket whatever address each is from; the
 * key keeps a peer from choosing ports that fill one bucket.
 * An IPv6 address's scope is left out too, since a connection bound to a
 * card is matched by its addresses without their scopes (`tcp_addr_is`).
 */
static uint32_t tcp_hash_index(uint16_t local_port, const net_addr_t *remote, uint16_t remote_port)
{
    uint32_t h = isn_key[0] ^ (((uint32_t)local_port << 16) | remote_port);
    uint32_t a = remote->v4;

    if (remote->family == AF_INET6) {
        a = read32be(remote->v6) ^ read32be(remote->v6 + 4) ^ read32be(remote->v6 + 8) ^
            read32be(remote->v6 + 12);
    }
    h ^= a * 0x9E3779B1u;
    h ^= h >> 15;
    h *= 0x85EBCA77u;
    h ^= isn_key[1];
    h ^= h >> 13;
    h *= 0xC2B2AE3Du;
    h ^= h >> 16;
    return h & (TCP_HASH_SIZE - 1);
}

/** Put `tcb`, a connection whose ports and addresses are set, in its bucket
 * of `tcp_hash`. */
static void tcb_hash_add(struct tcb *tcb)
{
    uint32_t i = tcp_hash_index(tcb->local_port, &tcb->remote_ip, tcb->remote_port);

    tcb->hash_next = tcp_hash[i];
    tcp_hash[i] = tcb;
    tcb->hashed = 1;
}

void tcp_bind_device(struct socket *sock, int ifindex)
{
    sock->bound_device = ifindex;
    if (sock->tcb) {
        sock->tcb->ifindex = ifindex;
    }
}

/** Take `tcb` out of `tcp_hash`, if it is there. */
static void tcb_hash_del(struct tcb *tcb)
{
    struct tcb **link;

    if (!tcb->hashed) {
        return;
    }
    link = &tcp_hash[tcp_hash_index(tcb->local_port, &tcb->remote_ip, tcb->remote_port)];
    while (*link && *link != tcb) {
        link = &(*link)->hash_next;
    }
    if (*link) {
        *link = tcb->hash_next;
    }
    tcb->hash_next = NULL;
    tcb->hashed = 0;
}

/**
 * Give `tcb` the local port `port`, counted in the table of ports in use as
 * `kind`, TCP_PORT_FIXED or TCP_PORT_AUTO, until `tcb_free`.
 */
static void tcb_take_port(struct tcb *tcb, uint16_t port, int kind)
{
    tcb->local_port = port;
    if (port == 0) {
        return;
    }
    tcb->port_held = (uint8_t)kind;
    if (kind == TCP_PORT_AUTO) {
        port_auto[port]++;
    } else {
        port_fixed[port]++;
    }
}

int tcp_port_in_use(uint16_t port)
{
    return port_fixed[port] != 0 || port_auto[port] != 0;
}

void tcp_port_hold(uint16_t port)
{
    if (port != 0) {
        port_fixed[port]++;
    }
}

void tcp_port_unhold(uint16_t port)
{
    if (port != 0 && port_fixed[port] > 0) {
        port_fixed[port]--;
    }
}

/** Take `tcb` off the list of control blocks. */
static void tcb_unlink(struct tcb *tcb)
{
    if (tcb == tw_first) {
        tw_first = tcb->next;
    }
    if (tcb->prev) {
        tcb->prev->next = tcb->next;
    } else if (tcbs == tcb) {
        tcbs = tcb->next;
    }
    if (tcb->next) {
        tcb->next->prev = tcb->prev;
    } else if (tcb_last == tcb) {
        tcb_last = tcb->prev;
    }
    tcb->next = tcb->prev = NULL;
    if (tcb->tw_listed) {
        tcb->tw_listed = 0;
        tw_count--;
    }
}

struct tcb *tcb_alloc(void)
{
    struct tcb *tcb = kzalloc(sizeof(*tcb));

    if (!tcb) {
        klog(KLOG_WARN, "tcp", "open: no memory for a control block");
        return NULL;
    }
    tcb_init(tcb);
    tcb->next = tcbs;
    if (tcbs) {
        tcbs->prev = tcb;
    } else {
        tcb_last = tcb;
    }
    tcbs = tcb;
    return tcb;
}

void tcb_free(struct tcb *tcb)
{
    if (!tcb) {
        return;
    }
    tcb_unlink(tcb);
    tcb_hash_del(tcb);
    if (tcb->batch_owed) {
        struct tcb **link = &tcp_batch_list;

        while (*link && *link != tcb) {
            link = &(*link)->batch_next;
        }
        if (*link) {
            *link = tcb->batch_next;
        }
        tcb->batch_owed = 0;
    }
    if (tcb->port_held == TCP_PORT_AUTO) {
        port_auto[tcb->local_port]--;
    } else if (tcb->port_held == TCP_PORT_FIXED) {
        port_fixed[tcb->local_port]--;
    }
    tcb->port_held = 0;
    tcb->in_use = 0;
    tcb->state = TCP_CLOSED;
    if (tcb->send_buf) {
        kfree(tcb->send_buf);
    }
    if (tcb->recv_buf) {
        kfree(tcb->recv_buf);
    }
    if (tcb->recv_seg_heap) {
        kfree(tcb->recv_seg_heap);
    }
    if (tcb->sent_more) {
        kfree(tcb->sent_more);
    }
    kfree(tcb);
}

/**
 * The checksum of the segment of `seg_len` bytes at `seg`, computed over a
 * 12-byte IPv4 or 40-byte IPv6 pseudo-header and the segment; 0 for a segment
 * received whose checksum is right.
 */
static uint16_t tcp_checksum(net_addr_t src_ip, net_addr_t dst_ip, const uint8_t *seg,
                             uint16_t seg_len)
{
    if (src_ip.family == AF_INET6) {
        return ip6_checksum(src_ip.v6, dst_ip.v6, IP6_NH_TCP, seg, seg_len);
    }
    write32be(tcp_cksum_buf + 0, src_ip.v4);
    write32be(tcp_cksum_buf + 4, dst_ip.v4);
    tcp_cksum_buf[8] = 0;
    tcp_cksum_buf[9] = 6;
    write16be(tcp_cksum_buf + 10, seg_len);
    k_memcpy(tcp_cksum_buf + 12, seg, seg_len);
    return ip_checksum(tcp_cksum_buf, (uint16_t)(12 + seg_len));
}

/** The index of the kept range that has sequence number `seq`, or -1. */
static int tcp_ooo_range_of(const struct tcb *tcb, uint32_t seq)
{
    int i;

    for (i = 0; i < tcb->ooo_count; i++) {
        if ((int32_t)(seq - tcb->ooo_start[i]) >= 0 && (int32_t)(seq - tcb->ooo_end[i]) < 0) {
            return i;
        }
    }
    return -1;
}

/**
 * Choose the kept ranges an acknowledgement from `tcb` names in its SACK
 * option, as indexes into `ooo_start` and `ooo_end` in `pick`: the ranges
 * of the last segments kept, the newest first, as RFC 2018 asks, and then
 * the other ranges in order, at most TCP_SACK_BLOCKS of them. Returns how
 * many.
 */
static uint8_t tcp_sack_pick(const struct tcb *tcb, uint8_t *pick)
{
    uint8_t n = 0, i, used = 0;
    int r;

    for (i = 0; i < tcb->sack_recent_count && n < TCP_SACK_BLOCKS; i++) {
        r = tcp_ooo_range_of(tcb, tcb->sack_recent[i]);
        if (r >= 0 && !(used & (1u << r))) {
            used |= (uint8_t)(1u << r);
            pick[n++] = (uint8_t)r;
        }
    }
    for (i = 0; i < tcb->ooo_count && n < TCP_SACK_BLOCKS; i++) {
        if (!(used & (1u << i))) {
            pick[n++] = i;
        }
    }
    return n;
}

/**
 * Write at `opt` the SACK option of an acknowledgement from `tcb`, which
 * holds data past a hole: the ranges `tcp_sack_pick` chooses, in its order.
 * Returns the bytes written, two NOPs with the option, a multiple of four.
 */
static uint8_t tcp_sack_option(const struct tcb *tcb, uint8_t *opt)
{
    uint8_t pick[TCP_SACK_BLOCKS];
    uint8_t n = tcp_sack_pick(tcb, pick), i;

    for (i = 0; i < n; i++) {
        write32be(opt + 4 + 8 * i, tcb->ooo_start[pick[i]]);
        write32be(opt + 8 + 8 * i, tcb->ooo_end[pick[i]]);
    }
    opt[0] = TCP_OPT_NOP;
    opt[1] = TCP_OPT_NOP;
    opt[2] = TCP_OPT_SACK;
    opt[3] = (uint8_t)(2 + 8 * n);
    return (uint8_t)(4 + 8 * n);
}

/**
 * Send the segment of `seg_len` bytes in `tcp_tx_buf` from `local_ip` to
 * `remote_ip` in the network namespace `ns`, by the card of index `ifindex`
 * or, for 0, the card the route chooses, counting it in `net_mib` when
 * IP takes it, and a reset among the resets. Returns what `ip_tx_opts`
 * returned, or -ENETUNREACH, counted in OutNoRoutes, when the machine no
 * longer has `local_ip`.
 *
 * A connection keeps its address when the address goes, as on Linux, whose
 * route lookup for a source the machine lacks fails with ENETUNREACH: its
 * segments are refused here until the address returns, and IPv4's output
 * is never reached, since it would put the machine's current source in
 * place of the one the checksum covers.
 */
static int tcp_ip_tx(const struct netns *ns, net_addr_t local_ip, net_addr_t remote_ip, int ifindex,
                     uint16_t seg_len)
{
    const struct netns *was = ip_ns;
    int rc;

    if (!net_addr_local(local_ip)) {
        if (local_ip.family == AF_INET6) {
            net_mib6.ip6_out_no_routes++;
        } else {
            net_mib.ip_out_no_routes++;
        }
        return -ENETUNREACH;
    }
    ip_ns = ns;
    if (remote_ip.family == AF_INET6) {
        rc = ip6_tx(local_ip.v6, remote_ip.v6, ifindex ? ifindex : (int)remote_ip.scope, IP6_NH_TCP,
                    tcp_tx_buf, seg_len, IP6_HOP_LIMIT_DEFAULT);
    } else {
        rc = ip_tx_opts(local_ip.v4, remote_ip.v4, IP_PROTO_TCP, tcp_tx_buf, seg_len,
                        IP_TTL_DEFAULT, IP_PMTUDISC_WANT, ifindex);
    }
    ip_ns = was;

    if (rc == 0) {
        net_mib.tcp_out_segs++;
        if (tcp_tx_buf[13] & TCP_RST) {
            net_mib.tcp_out_rsts++;
        }
    }
    return rc;
}

/**
 * Send the segment of `seg_len` bytes in `tcp_tx_buf` on `tcb`'s addresses
 * (`tcp_ip_tx`), and keep a refusal for want of a route as the soft error
 * the connection's give-up reports. A segment IP took that acknowledges
 * moves `rcv_acked` and `rcv_offered`, ends a held acknowledgement and,
 * when data or a FIN came since the last, uses one of the quick ones, as
 * Linux's tcp_event_ack_sent does; one that carries data within
 * TCP_DELACK_TICKS of data received is an answer, and puts the connection
 * in ping-pong mode, as Linux's tcp_event_data_sent does. Returns what
 * `tcp_ip_tx` returned.
 */
static int tcp_tcb_tx(struct tcb *tcb, uint16_t seg_len)
{
    int rc = tcp_ip_tx(tcb->ns, tcb->local_ip, tcb->remote_ip, tcb->ifindex, seg_len);

    if (rc == -ENETUNREACH || rc == -EHOSTUNREACH) {
        tcb->soft_error = (uint32_t)-rc;
    }
    if (rc == 0 && (tcp_tx_buf[13] & TCP_ACK) && !(tcp_tx_buf[13] & TCP_RST)) {
        uint32_t field = read16be(tcp_tx_buf + 14);

        if (tcb->ack_sched && tcb->ack_quick) {
            tcb->ack_quick--;
        }
        tcb->ack_sched = 0;
        tcb->rcv_acked = read32be(tcp_tx_buf + 8);
        tcb->rcv_offered = (tcp_tx_buf[13] & TCP_SYN) ? field : field << tcb->rcv_wscale;
        tcb->delack_due = 0;
        tcb->ack_pushed = 0;
        if (seg_len > (uint16_t)((tcp_tx_buf[12] >> 4) * 4u) && tcb->rcv_data_seen &&
            TCP_NOW() - tcb->rcv_data_at < TCP_DELACK_TICKS) {
            tcb->pingpong = 1;
        }
    }
    return rc;
}

/**
 * Count a connection a reset or a timeout ends in `state`, as Linux counts
 * one: a handshake as a failed attempt, an open connection as a reset.
 */
static void tcp_count_end(int state)
{
    if (state == TCP_SYN_SENT || state == TCP_SYN_RCVD) {
        net_mib.tcp_attempt_fails++;
    } else if (state == TCP_ESTABLISHED || state == TCP_CLOSE_WAIT) {
        net_mib.tcp_estab_resets++;
    }
}

/**
 * Build and send a segment of `tcb` with no data, with the control `flags`
 * at sequence `seq`, without moving `snd_nxt` or arming a retransmission.
 * A SYN has the options this side offers, and an acknowledgement sent
 * while data past a hole is kept names that data in a SACK option. Returns
 * what `tcp_ip_tx` returned; an acknowledgement IP took clears `ack_owed`.
 */
static int tcp_emit_flags(struct tcb *tcb, uint8_t flags, uint32_t seq)
{
    uint8_t hdr_len;
    uint16_t seg_len;
    int rc;

    hdr_len = 20;
    if (flags & TCP_SYN) {
        hdr_len = (uint8_t)(24 + (tcb->ws_offer ? 4 : 0) + (tcb->sack_offer ? 4 : 0));
    } else if ((flags & TCP_ACK) && !(flags & TCP_RST) && tcb->sack_ok && tcb->ooo_count > 0) {
        hdr_len = (uint8_t)(20 + tcp_sack_option(tcb, tcp_tx_buf + 20));
    }

    write16be(tcp_tx_buf + 0, tcb->local_port);
    write16be(tcp_tx_buf + 2, tcb->remote_port);
    write32be(tcp_tx_buf + 4, seq);
    write32be(tcp_tx_buf + 8, (flags & TCP_ACK) ? tcb->rcv_nxt : 0);
    tcp_tx_buf[12] = (uint8_t)((hdr_len >> 2) << 4);
    tcp_tx_buf[13] = flags;
    write16be(tcp_tx_buf + 14, tcp_wnd_field(tcb, flags));
    write16be(tcp_tx_buf + 16, 0);
    write16be(tcp_tx_buf + 18, 0);

    if (flags & TCP_SYN) {
        tcp_tx_buf[20] = TCP_OPT_MSS;
        tcp_tx_buf[21] = 4;
        write16be(tcp_tx_buf + 22, tcp_offer_mss(tcb));
    }
    if ((flags & TCP_SYN) && tcb->ws_offer) {
        tcp_tx_buf[24] = TCP_OPT_NOP;
        tcp_tx_buf[25] = TCP_OPT_WS;
        tcp_tx_buf[26] = 3;
        tcp_tx_buf[27] = tcb->rcv_wscale;
    }
    if ((flags & TCP_SYN) && tcb->sack_offer) {
        uint8_t at = tcb->ws_offer ? 28 : 24;

        tcp_tx_buf[at] = TCP_OPT_NOP;
        tcp_tx_buf[at + 1] = TCP_OPT_NOP;
        tcp_tx_buf[at + 2] = TCP_OPT_SACK_OK;
        tcp_tx_buf[at + 3] = 2;
    }

    seg_len = hdr_len;
    write16be(tcp_tx_buf + 16, tcp_checksum(tcb->local_ip, tcb->remote_ip, tcp_tx_buf, seg_len));
    rc = tcp_tcb_tx(tcb, seg_len);
    if (rc == 0 && (flags & TCP_ACK)) {
        tcb->ack_owed = 0;
    }
    return rc;
}

/**
 * Copy the `n` bytes at `src` into the ring of `size` bytes at the count
 * `at`, in at most two calls.
 *
 * The size is a power of two and the index wraps through a mask, so a run
 * that crosses the end is two straight runs, each copied by `k_memcpy`, which
 * moves words; a byte loop would cost a load, a store and a mask for every
 * byte of every segment.
 */
static void ring_put(uint8_t *ring, uint32_t size, uint32_t at, const uint8_t *src, uint32_t n)
{
    uint32_t head = at & (size - 1);
    uint32_t first = size - head;

    if (first > n) {
        first = n;
    }
    k_memcpy(ring + head, src, first);
    if (n > first) {
        k_memcpy(ring, src + first, n - first);
    }
}

/** Copy `n` bytes out of the ring of `size` bytes at the count `at` into
 * `dst`, as `ring_put` copies them in. */
static void ring_get(uint8_t *dst, const uint8_t *ring, uint32_t size, uint32_t at, uint32_t n)
{
    uint32_t head = at & (size - 1);
    uint32_t first = size - head;

    if (first > n) {
        first = n;
    }
    k_memcpy(dst, ring + head, first);
    if (n > first) {
        k_memcpy(dst + first, ring, n - first);
    }
}

/** The runs of `tcb`'s send history, in `sent_more` when it has one. */
static struct tcp_sent_run *tcp_sent(struct tcb *tcb)
{
    return tcb->sent_more ? tcb->sent_more : tcb->sent_first;
}

/**
 * Move the send history of `tcb`, which has one run free, to a heap array
 * twice as large (`tcp_sent_room`): 1 when it was moved, 0 when the heap
 * refused, which leaves it as it was. The runs in flight are at most
 * the send ring's bytes and the FIN, so the array stays below twice that.
 */
static int tcp_sent_grow(struct tcb *tcb)
{
    uint32_t cap = (tcb->sent_more ? tcb->sent_cap : TCP_SENT_RUNS) * 2u;
    struct tcp_sent_run *more = kmalloc(cap * sizeof(*more));

    if (!more) {
        return 0;
    }
    k_memcpy(more, tcp_sent(tcb), tcb->sent_runs * sizeof(*more));
    if (tcb->sent_more) {
        kfree(tcb->sent_more);
    }
    tcb->sent_more = more;
    tcb->sent_cap = cap;
    return 1;
}

/**
 * Whether bytes `tcb` sends now for the first time can be noted in its send
 * history at this tick, with a run left free after them for the FIN: when
 * they join the newest run, sent this tick, or a run of their own leaves
 * one free, the history growing for it when it would not
 * (`tcp_sent_grow`). Linux stamps each segment it sends, so no byte's
 * stamp may be another's: `tcp_output` sends no new data when this says
 * no, and the tick's pump sends it later. The FIN, the last byte a
 * connection sends, always finds its run.
 */
static int tcp_sent_room(struct tcb *tcb)
{
    uint32_t n = tcb->sent_runs;
    uint32_t cap = tcb->sent_more ? tcb->sent_cap : TCP_SENT_RUNS;

    if (n > 0 && tcp_sent(tcb)[n - 1].tick == tcp_due(1)) {
        return 1;
    }
    return n + 1 < cap || tcp_sent_grow(tcb);
}

/**
 * Note that the bytes of `tcb` before `end` went out this tick for the first
 * time, in its send history: they join the newest run when it was sent this
 * tick, and otherwise start a run of their own, which `tcp_sent_room` has
 * kept free.
 */
static void tcp_sent_note(struct tcb *tcb, uint32_t end)
{
    uint32_t stamp = tcp_due(1); /* now + 1, never 0 */
    uint32_t n = tcb->sent_runs;
    uint32_t cap = tcb->sent_more ? tcb->sent_cap : TCP_SENT_RUNS;
    struct tcp_sent_run *run = tcp_sent(tcb);

    if (n > 0 && (run[n - 1].tick == stamp || n == cap)) {
        run[n - 1].end = end; /* n == cap: never, kept so as not to overrun */
        run[n - 1].tick = stamp;
        return;
    }
    run[n].end = end;
    run[n].tick = stamp;
    tcb->sent_runs = n + 1;
}

/** Drop the runs of `tcb`'s send history that `snd_una` has passed, which
 * an acknowledgement has covered whole, and free `sent_more` once none is
 * left. */
static void tcp_sent_acked(struct tcb *tcb)
{
    struct tcp_sent_run *run = tcp_sent(tcb);
    uint32_t gone = 0, i;

    while (gone < tcb->sent_runs && (int32_t)(run[gone].end - tcb->snd_una) <= 0) {
        gone++;
    }
    for (i = gone; gone > 0 && i < tcb->sent_runs; i++) {
        run[i - gone] = run[i];
    }
    tcb->sent_runs -= gone;
    if (tcb->sent_runs == 0 && tcb->sent_more) {
        kfree(tcb->sent_more);
        tcb->sent_more = NULL;
        tcb->sent_cap = 0;
    }
}

/** Take `window` as the peer's window, and as the largest it has offered
 * when it is (`snd_wnd_max`). */
static void tcp_take_wnd(struct tcb *tcb, uint32_t window)
{
    tcb->snd_wnd = window;
    if (window > tcb->snd_wnd_max) {
        tcb->snd_wnd_max = window;
    }
}

/**
 * Send a control segment at snd_nxt. SYN and FIN consume a sequence number and
 * are marked for retransmission until acknowledged.
 *
 * Retry a bare ACK on the next tick only when transmission returns `-ENOBUFS`.
 * Each retry uses the current receive state. Other errors clear `ack_owed`;
 * they neither start a retransmission timer nor close the connection.
 */
static void tcp_send_flags(struct tcb *tcb, uint8_t flags)
{
    uint32_t seq = tcb->snd_nxt;
    int rc = tcp_emit_flags(tcb, flags, seq);

    if (!(flags & (TCP_SYN | TCP_FIN))) {
        tcb->ack_owed = (rc == -ENOBUFS) ? 1 : 0;
        if (tcb->ack_owed && tcb->tw_listed) {
            tw_owed = 1;
        }
    }

    if (flags & (TCP_SYN | TCP_FIN)) {
        tcb->retx_flags = flags;
        tcb->retx_seq = seq;
        if (tcb->rto_due == 0) {
            tcb->rto_due = tcp_due(tcb->rto);
        }
    }
    if (flags & TCP_SYN)
        tcb->snd_nxt++;
    if (flags & TCP_FIN) {
        tcb->snd_nxt++;
        tcp_sent_note(tcb, tcb->snd_nxt);
    }
}

/**
 * Start the zero-window probe timer when the peer's window is closed over
 * bytes this side still has to send or has sent unacknowledged, and the
 * timer is not running. A window update lost on the way is then found by
 * the probe rather than waited for. An open window stops the timer and
 * ends the probing's back-off and its run, as Linux's tcp_ack_probe does;
 * the caller calls this after each change of the window.
 */
static void tcp_arm_zwp(struct tcb *tcb)
{
    if (tcb->snd_wnd != 0) {
        tcb->zwp_due = 0;
        tcb->zwp_backoff = 0;
        tcb->zwp_stamp = 0;
    } else if (tcb->zwp_due == 0 && tcb->send_tail != tcb->send_ack) {
        tcb->zwp_due = tcp_due(tcb->rto);
    }
}

/**
 * The size of the probe `tcp_output` may send now with `avail` bytes queued
 * and `usable` bytes of the windows free, or 0 (TCP_MTU_PROBING_DEFAULT):
 * halfway from the segment size to the top of the search, once the search
 * has started, in ESTABLISHED outside a loss recovery, with no probe in
 * flight, from the tick `probe_after` on, and with the probe and
 * TCP_DUP_ACKS segments after it both queued and inside the windows, since
 * those segments' acknowledgements tell the probe's loss without a
 * timeout. A search closed to within TCP_MTU_PROBE_THRESH bytes below the
 * top starts again from `mss_max` after TCP_PROBE_INTERVAL.
 */
static uint32_t tcp_probe_due(struct tcb *tcb, uint32_t avail, uint32_t usable)
{
    uint32_t size, need;

    if (!tcb->mtup_on || tcp_mtu_probing == 0 || tcb->probe_size != 0 || tcb->recovering ||
        tcb->state != TCP_ESTABLISHED || tcb->snd_mss >= tcb->mss_max ||
        (int32_t)(TCP_NOW() - tcb->probe_after) < 0) {
        return 0;
    }
    if ((uint32_t)tcb->mss_hi < (uint32_t)tcb->snd_mss + TCP_MTU_PROBE_THRESH) {
        tcb->mss_hi = tcb->mss_max;
        tcb->probe_after = TCP_NOW() + TCP_PROBE_INTERVAL;
        return 0;
    }
    size = ((uint32_t)tcb->snd_mss + tcb->mss_hi + 1u) / 2u;
    need = size + TCP_DUP_ACKS * (uint32_t)tcb->snd_mss;
    return avail >= need && usable >= need ? size : 0;
}

/**
 * End the probe in flight when a loss recovery begins: when the loss is the
 * probe's own, at the oldest byte unacknowledged, the path did not carry
 * it, and the top of the search falls below it; a loss before it says
 * nothing of its size, and the probe is only forgotten, since the resends
 * that follow would cover it with smaller segments. The next probe may go
 * at once.
 */
static void tcp_probe_end_lost(struct tcb *tcb)
{
    if (tcb->probe_size == 0) {
        return;
    }
    if (tcb->snd_una == tcb->probe_seq) {
        tcb->mss_hi = (uint16_t)(tcb->probe_size - 1u);
        net_mib.tcp_mtup_fail++;
    }
    tcb->probe_size = 0;
    tcb->probe_after = TCP_NOW();
}

/**
 * Lower the segment size at a retransmission timeout that may be a path
 * MTU black hole (TCP_MTU_PROBING_DEFAULT): the TCP_RETRIES1th timeout in
 * a row or a later one, after the handshake, with more than the floor
 * unacknowledged, of a segment the card took on a link that is up, since a
 * refused frame says nothing of the path. The first starts the search and
 * lowers the size to TCP_BASE_MSS, each later one halves it, never below
 * `tcp_mss_floor`, and the next probe waits TCP_PROBE_INTERVAL.
 */
static void tcp_blackhole(struct tcb *tcb)
{
    uint32_t sent_end = tcb->snd_una + (tcb->send_head - tcb->send_ack);
    uint32_t floor = tcp_mss_floor(tcb);
    uint32_t mss;

    if (tcp_mtu_probing == 0 || tcb->rto_backoff < TCP_RETRIES1 || tcb->refused_since != 0 ||
        tcb->state < TCP_ESTABLISHED || tcb->snd_mss <= floor || sent_end - tcb->snd_una <= floor ||
        net_addr_link_down(tcb->remote_ip, tcb->ifindex)) {
        return;
    }
    mss = tcb->mtup_on ? tcb->snd_mss / 2u : TCP_BASE_MSS;
    if (mss > tcb->snd_mss)
        mss = tcb->snd_mss;
    if (mss < floor)
        mss = floor;
    tcb->mtup_on = 1;
    tcb->mss_hi = tcb->mss_max;
    tcb->probe_after = TCP_NOW() + TCP_PROBE_INTERVAL;
    if (mss < tcb->snd_mss) {
        klog(KLOG_INFO, "tcp", "black hole: to port %d: segments of %d bytes",
             (int)tcb->remote_port, (int)mss);
        tcb->snd_mss = (uint16_t)mss;
    }
}

/**
 * Send new data from the send ring: as many segments as the peer's and the
 * congestion windows allow, less the bytes already in flight, so that a
 * response of several segments leaves in one burst and not one segment a
 * round trip. A window that allows none starts the zero-window probe.
 *
 * A segment the driver would not take (`tcp_ip_tx` returns -ENOBUFS) stays in
 * the ring unsent, for the next pump: an acknowledgement, or the tick, which
 * gives up on a connection whose bytes have been refused since
 * TCP_REFUSE_GIVEUP_TICKS ago with nothing sent between. Counted as sent it
 * would wait for a retransmission a second away. Any other refusal is
 * permanent, and the segment is counted as sent so that the retransmission
 * timer and its give-up take the connection down. A segment sent also
 * acknowledges, which clears `ack_owed`. Data whose send the history
 * cannot note at its own tick, since the heap refused it a larger array
 * (`tcp_sent_room`), stays in the ring for the tick's pump too.
 *
 * One round-trip sample runs at a time, on data sent for the first time
 * outside a loss recovery (Karn's algorithm); the acknowledgement that covers
 * the segment ends it. A segment may be a probe of a larger size
 * (`tcp_probe_due`), whose place is kept until it is acknowledged or lost.
 *
 * Returns 1 when it stopped on a refusal, 0 when it stopped for any other
 * reason: nothing queued, a window that allows no more, or a send history
 * with no run free.
 */
static int tcp_output(struct tcb *tcb)
{
    uint32_t avail, in_flight, window, usable, send_len, probe, most;
    uint16_t seg_len;

    tcp_mss_clamp(tcb);
    for (;;) {
        avail = tcb->send_tail - tcb->send_head;
        if (avail == 0) {
            return 0;
        }

        in_flight = tcb->snd_nxt - tcb->snd_una;

        window = tcb->snd_wnd;
        if (tcb->cwnd < window) {
            window = tcb->cwnd;
        }
        usable = (window > in_flight) ? (window - in_flight) : 0;

        probe = tcp_probe_due(tcb, avail, usable);
        most = probe ? probe : tcb->snd_mss;
        send_len = avail;
        if (send_len > most)
            send_len = most;
        if (send_len > usable)
            send_len = usable;

        if (send_len == 0) {
            tcp_arm_zwp(tcb);
            return 0;
        }
        if (!tcp_sent_room(tcb)) {
            return 0;
        }

        write16be(tcp_tx_buf + 0, tcb->local_port);
        write16be(tcp_tx_buf + 2, tcb->remote_port);
        write32be(tcp_tx_buf + 4, tcb->snd_nxt);
        write32be(tcp_tx_buf + 8, tcb->rcv_nxt);
        tcp_tx_buf[12] = (uint8_t)((TCP_HDR_SIZE >> 2) << 4);
        tcp_tx_buf[13] = TCP_ACK | TCP_PSH;
        write16be(tcp_tx_buf + 14, tcp_wnd_field(tcb, TCP_ACK));
        write16be(tcp_tx_buf + 16, 0);
        write16be(tcp_tx_buf + 18, 0);

        ring_get(tcp_tx_buf + TCP_HDR_SIZE, tcb->send_buf, TCP_RING_SIZE, tcb->send_head, send_len);

        seg_len = (uint16_t)(TCP_HDR_SIZE + send_len);
        write16be(tcp_tx_buf + 16,
                  tcp_checksum(tcb->local_ip, tcb->remote_ip, tcp_tx_buf, seg_len));
        if (tcp_tcb_tx(tcb, seg_len) == -ENOBUFS) {
            tcb->tx_refused++;
            if (tcb->refused_since == 0) {
                tcb->refused_since = TCP_NOW() + 1;
            }
            return 1;
        }
        tcb->refused_since = 0;
        tcb->ack_owed = 0;
        if (probe) {
            tcb->probe_size = (uint16_t)probe;
            tcb->probe_seq = tcb->snd_nxt;
        }

        tcb->send_head += send_len;
        tcb->snd_nxt += send_len;
        tcp_sent_note(tcb, tcb->snd_nxt);

        if (tcb->rto_due == 0) {
            tcb->rto_due = tcp_due(tcb->rto);
        }
        if (tcb->rtt_start == 0 && !tcb->recovering) {
            tcb->rtt_start = TCP_NOW() + 1;
            tcb->rtt_start_us = timer_now_us();
            tcb->rtt_seq = tcb->snd_nxt;
        }
    }
}

/**
 * Send one segment of already-sent data again: up to an MSS from `seq`,
 * bounded by the data sent and by `most`. Unlike tcp_output this reads the
 * ring at the acknowledged position plus the distance from snd_una, and
 * leaves snd_nxt and send_head untouched, so snd_nxt stays the true
 * high-water mark of transmitted data: an ACK for anything already sent
 * therefore still satisfies the (ack_num <= snd_nxt) window test and
 * advances the connection instead of being rejected.
 *
 * The first attempt since the last recovery ended, a fast retransmission's,
 * a timeout's or a probe's, sent or refused, stamps `retx_stamp`, as
 * Linux's tcp_retransmit_skb stamps retrans_stamp; an acknowledgement that
 * ends the recovery clears it (`tcp_take_ack`).
 *
 * A SYN still unacknowledged at snd_una (`syn_unacked`) is sent again as
 * the SYN-ACK, alone, which counts as one byte sent, as Linux's
 * tcp_retransmit_skb sends the head of its queue.
 *
 * Returns the bytes sent; 0 when `seq` is at or past the data sent or `most`
 * is 0; -1 when the driver refused the segment, counted as tcp_output counts
 * a refusal so that the tick's pump sends it later.
 */
static int tcp_resend(struct tcb *tcb, uint32_t seq, uint32_t most)
{
    uint32_t sent_end = tcb->snd_una + (tcb->send_head - tcb->send_ack);
    uint32_t from = tcb->send_ack + (seq - tcb->snd_una);
    uint32_t send_len, i;
    uint16_t seg_len;
    int rc;

    if ((int32_t)(sent_end - seq) <= 0 || most == 0) {
        return 0;
    }
    if (tcb->retx_stamp == 0) {
        tcb->retx_stamp = tcp_due(1); /* this tick + 1 */
    }
    if (tcb->syn_unacked && seq == tcb->snd_una) {
        rc = tcp_emit_flags(tcb, TCP_SYN | TCP_ACK, seq);
        send_len = 1;
    } else {
        tcp_mss_clamp(tcb);
        send_len = sent_end - seq;
        if (send_len > tcb->snd_mss)
            send_len = tcb->snd_mss;
        if (send_len > most)
            send_len = most;

        write16be(tcp_tx_buf + 0, tcb->local_port);
        write16be(tcp_tx_buf + 2, tcb->remote_port);
        write32be(tcp_tx_buf + 4, seq);
        write32be(tcp_tx_buf + 8, tcb->rcv_nxt);
        tcp_tx_buf[12] = (uint8_t)((TCP_HDR_SIZE >> 2) << 4);
        tcp_tx_buf[13] = TCP_ACK | TCP_PSH;
        write16be(tcp_tx_buf + 14, tcp_wnd_field(tcb, TCP_ACK));
        write16be(tcp_tx_buf + 16, 0);
        write16be(tcp_tx_buf + 18, 0);

        for (i = 0; i < send_len; i++) {
            tcp_tx_buf[TCP_HDR_SIZE + i] = tcb->send_buf[(from + i) & (TCP_RING_SIZE - 1)];
        }

        seg_len = (uint16_t)(TCP_HDR_SIZE + send_len);
        write16be(tcp_tx_buf + 16,
                  tcp_checksum(tcb->local_ip, tcb->remote_ip, tcp_tx_buf, seg_len));
        rc = tcp_tcb_tx(tcb, seg_len);
    }
    if (rc == -ENOBUFS) {
        tcb->tx_refused++;
        if (tcb->refused_since == 0) {
            tcb->refused_since = TCP_NOW() + 1;
        }
        return -1;
    }
    tcb->refused_since = 0;
    tcb->ack_owed = 0;
    tcb->retransmits++;
    net_mib.tcp_retrans_segs++;
    return (int)send_len;
}

/**
 * After a timeout, send the unacknowledged data again from `retx_nxt` up to
 * `recover`, as far as the windows allow with the bytes sent again counted
 * as the bytes in flight: everything else in flight is presumed lost (RFC
 * 5681, section 3.1). Each acknowledgement grows the congestion window and
 * comes back here through tcp_pump, so data lost in a burst is on the wire
 * again within a few round trips, one segment, then two, then four, rather
 * than one segment per timeout. A window with no room stops it; a zero
 * window is probed from the acknowledgement path. Returns 1 when the driver
 * refused a segment, which the tick's pump retries.
 */
static int tcp_recover_send(struct tcb *tcb)
{
    while (tcb->recovering == TCP_RECOVER_RTO && (int32_t)(tcb->retx_nxt - tcb->recover) < 0) {
        uint32_t window = tcb->snd_wnd;
        uint32_t out = tcb->retx_nxt - tcb->snd_una;
        int n;

        if (tcb->cwnd < window)
            window = tcb->cwnd;
        if (out >= window) {
            return 0;
        }
        n = tcp_resend(tcb, tcb->retx_nxt, window - out);
        if (n < 0) {
            return 1;
        }
        if (n == 0) {
            return 0;
        }
        tcb->retx_nxt += (uint32_t)n;
    }
    return 0;
}

/**
 * Begin recovery after a timeout (RFC 5681, section 3.1): the threshold is
 * half of what was in flight, at least two segments; the congestion window
 * is one segment; the resend starts at the oldest unacknowledged byte. In a
 * recovery already under way the flight is the congestion window, since the
 * data beyond it was presumed lost when that recovery began. No round-trip
 * sample is taken from data sent again (Karn's algorithm). The caller has
 * doubled the timeout, and it is restarted here.
 */
static void tcp_enter_rto_recovery(struct tcb *tcb)
{
    uint32_t sent_end = tcb->snd_una + (tcb->send_head - tcb->send_ack);
    uint32_t flight = tcb->recovering ? tcb->cwnd : sent_end - tcb->snd_una;
    uint32_t new_ss = flight / 2u;

    tcp_probe_end_lost(tcb);
    if (new_ss < 2u * tcb->snd_mss)
        new_ss = 2u * tcb->snd_mss;
    tcb->ssthresh = new_ss;
    tcb->cwnd = tcb->snd_mss;
    tcb->recovering = TCP_RECOVER_RTO;
    tcb->recover = sent_end;
    tcb->retx_nxt = tcb->snd_una;
    tcb->dup_acks = 0;
    tcb->rtt_start = 0;
    (void)tcp_recover_send(tcb);
    tcb->rto_due = tcp_due(tcb->rto);
}

/**
 * Begin recovery at the third duplicate acknowledgement (RFC 5681, section
 * 3.2; RFC 6582): the segment the peer keeps asking for is sent again at
 * once, without waiting for the timer, and a resend the driver refuses is
 * owed in `fast_pending`; the threshold is half of what is in flight, at
 * least two segments; the congestion window is the threshold plus the three
 * segments the duplicates say have left the network. Each further duplicate
 * adds a segment, a partial acknowledgement sends the next hole again, and
 * the acknowledgement of everything sent before the loss ends the recovery
 * at the threshold.
 */
static void tcp_enter_fast_recovery(struct tcb *tcb)
{
    uint32_t sent_end = tcb->snd_una + (tcb->send_head - tcb->send_ack);
    uint32_t new_ss = (sent_end - tcb->snd_una) / 2u;

    tcp_probe_end_lost(tcb);
    if (new_ss < 2u * tcb->snd_mss)
        new_ss = 2u * tcb->snd_mss;
    tcb->ssthresh = new_ss;
    tcb->recovering = TCP_RECOVER_FAST;
    tcb->recover = sent_end;
    tcb->rtt_start = 0;
    tcb->fast_pending = tcp_resend(tcb, tcb->snd_una, tcb->snd_mss) < 0;
    tcb->cwnd = new_ss + TCP_DUP_ACKS * tcb->snd_mss;
}

/** The connection `seq` is in flight on, from `local_ip`:`local_port` to
 * `remote_ip`:`remote_port`, or NULL as `tcp_in_flight` says. */
static struct tcb *tcp_flight(net_addr_t local_ip, uint16_t local_port, net_addr_t remote_ip,
                              uint16_t remote_port, uint32_t seq)
{
    struct tcb *tcb = tcp_find(local_ip, local_port, remote_ip, remote_port);

    if (!tcb || tcb->state < TCP_ESTABLISHED || (int32_t)(seq - tcb->snd_una) < 0 ||
        (int32_t)(seq - tcb->snd_nxt) >= 0) {
        return NULL;
    }
    return tcb;
}

int tcp_in_flight(net_addr_t local_ip, uint16_t local_port, net_addr_t remote_ip,
                  uint16_t remote_port, uint32_t seq)
{
    return tcp_flight(local_ip, local_port, remote_ip, remote_port, seq) != NULL;
}

void tcp_path_mtu(uint32_t local_ip, uint32_t remote_ip, uint16_t local_port, uint16_t remote_port,
                  uint32_t seq, uint16_t mtu)
{
    struct tcb *tcb =
        tcp_flight(net_addr4(local_ip), local_port, net_addr4(remote_ip), remote_port, seq);
    uint32_t mss = mtu > 40u ? mtu - 40u : 0;

    if (!tcb) {
        return;
    }
    if (mss < TCP_MSS_DEFAULT) {
        mss = TCP_MSS_DEFAULT;
    }
    if (mss < tcb->mss_max) {
        tcb->mss_max = (uint16_t)mss;
    }
    if (mss < tcb->mss_hi) {
        tcb->mss_hi = (uint16_t)mss;
    }
    if (mss >= tcb->snd_mss) {
        return;
    }
    klog(KLOG_INFO, "tcp", "path mtu: %d to port %d: segments of %d bytes", (int)mtu,
         (int)remote_port, (int)mss);
    tcb->snd_mss = (uint16_t)mss;
    tcp_enter_rto_recovery(tcb);
}

/**
 * Send what a connection can: first the hole a fast recovery owes when the
 * driver refused its resend (`fast_pending`), then the data a timeout's
 * recovery sends again, then new data. Returns 1 when the driver refused a
 * segment.
 */
static int tcp_send_more(struct tcb *tcb)
{
    if (tcb->fast_pending) {
        if (tcp_resend(tcb, tcb->snd_una, tcb->snd_mss) < 0) {
            return 1;
        }
        tcb->fast_pending = 0;
    }
    if (tcp_recover_send(tcb)) {
        return 1;
    }
    return tcp_output(tcb);
}

/**
 * Send queued data, and once the send ring has drained, the FIN a close or
 * a shutdown left pending, which moves ESTABLISHED to FIN_WAIT_1 and
 * CLOSE_WAIT to LAST_ACK. A close so completes even when its data did not
 * fit in one window: the acknowledgements open the window, send more data,
 * and send the FIN after the last byte.
 */
static void tcp_pump(struct tcb *tcb)
{
    (void)tcp_send_more(tcb);

    if (tcb->fin_pending && tcb->send_head == tcb->send_tail) {
        tcb->fin_pending = 0;
        tcp_send_flags(tcb, TCP_FIN | TCP_ACK);
        if (tcb->state == TCP_ESTABLISHED) {
            tcb->state = TCP_FIN_WAIT_1;
        } else if (tcb->state == TCP_CLOSE_WAIT) {
            tcb->state = TCP_LAST_ACK;
        }
    }
}

int tcp_send(struct socket *sock, const uint8_t *buf, int len)
{
    struct tcb *tcb;
    uint32_t free_space, copy_len, flags;

    flags = net_lock();
    if (!sock || !sock->tcb || len < 0) {
        net_unlock(flags);
        return -1;
    }
    tcb = sock->tcb;

    if ((tcb->state != TCP_ESTABLISHED && tcb->state != TCP_CLOSE_WAIT) || tcb->fin_pending ||
        (sock->shutdown & SOCK_SEND_SHUTDOWN)) {
        int rc =
            tcb->state == TCP_SYN_SENT || tcb->state == TCP_SYN_RCVD ? TCP_SEND_CONNECTING : -1;

        net_unlock(flags);
        return rc;
    }

    free_space = TCP_RING_SIZE - (tcb->send_tail - tcb->send_ack);
    copy_len = (uint32_t)len;
    if (copy_len > free_space)
        copy_len = free_space;

    if (copy_len > 0) {
        ring_put(tcb->send_buf, TCP_RING_SIZE, tcb->send_tail, buf, copy_len);
        tcb->send_tail += copy_len;
        tcp_output(tcb);
    }

    net_unlock(flags);
    return (int)copy_len;
}

/**
 * The span used to choose an offset, count probes and subtract on wrapping
 * in a TCP port search over the packed `range`. As in Linux 6.12, the span
 * is rounded down to an even count unless it is 1. The search wraps at
 * the range's unrounded exclusive upper bound.
 */
static uint32_t tcp_port_span(uint32_t range)
{
    uint32_t span = NET_PORT_HIGH(range) + 1u - NET_PORT_LOW(range);

    return span > 1u ? span & ~1u : span;
}

/**
 * A port of the ephemeral range that no block and no bound stream socket
 * holds, or 0 when none is free: Linux's inet_csk_find_open_port, which
 * starts at a random place and tries the ports of that odd offset from the
 * range's low end first, then the even ones, leaving the even ones to
 * `tcp_connect_port`. The place is a keyed hash of a count of the
 * searches, which no host off the machine can predict. The caller holds
 * `net_lock`.
 */
static uint16_t tcp_free_port(void)
{
    net_addr_t none = net_addr4(0), count;
    uint32_t range = net_port_range;
    uint32_t low = NET_PORT_LOW(range), high = NET_PORT_HIGH(range) + 1u;
    uint32_t span = tcp_port_span(range), offset, i, port;

    tcp_isn_key();
    count = net_addr4(bind_searches++);
    offset = (tcp_tuple_hash(&none, &count, 0, 0) % span) | 1u;
    for (;;) {
        port = low + offset;
        for (i = 0; i < span; i += 2, port += 2) {
            if (port >= high) {
                port -= span;
            }
            if (port_fixed[port] == 0 && port_auto[port] == 0) {
                return (uint16_t)port;
            }
        }
        if (!(offset & 1u)) {
            return 0;
        }
        offset--;
    }
}

/**
 * The port for a connection of the network namespace `ns` from `from` to
 * `dst`:`dst_port` that a socket with no port opens, or 0 when none is free,
 * chosen as Linux's __inet_hash_connect chooses it (RFC 6056, algorithm 4). The
 * search starts where the last one to this destination ended, a keyed hash of
 * the addresses and the peer's port picking the place in `port_perturb` that
 * keeps it, tries the ports an even offset from the range's low end first, then
 * the others, and takes the first port that no listener, passive connection or
 * bound socket holds and no connection from `from` has to this peer already. So
 * ports can be shared among connections to different peers, and a block in
 * TIME_WAIT keeps its port from the next connection to its peer.
 * The place moves past the port,
 * and a little further by the hash of the chosen pair, as Linux moves it by a
 * random step, so that its next value is not plain to the peer.
 * `ifindex` is the card the connection is bound to (`tcp_pair_in_use`).
 * The caller holds `net_lock`.
 */
static uint16_t tcp_connect_port(const struct netns *ns, net_addr_t from, net_addr_t dst,
                                 uint16_t dst_port, int ifindex)
{
    uint32_t h = tcp_tuple_hash(&from, &dst, 0, dst_port);
    uint32_t place = h & (TCP_PERTURB_SIZE - 1u);
    uint32_t range = net_port_range;
    uint32_t low = NET_PORT_LOW(range), high = NET_PORT_HIGH(range) + 1u;
    uint32_t span = tcp_port_span(range);
    uint32_t offset = ((port_perturb[place] + (h >> 8)) % span) & ~1u;
    uint32_t i, step, port;

    for (;;) {
        port = low + offset;
        for (i = 0; i < span; i += 2, port += 2) {
            if (port >= high) {
                port -= span;
            }
            if (port_fixed[port] != 0 ||
                (port_auto[port] != 0 &&
                 tcp_pair_in_use(ns, from, (uint16_t)port, dst, dst_port, ifindex))) {
                continue;
            }
            step = (tcp_tuple_hash(&from, &dst, (uint16_t)port, dst_port) >> 29) * 2u;
            port_perturb[place] += (i > step ? i : step) + 2u;
            return (uint16_t)port;
        }
        offset++;
        if (!(offset & 1u) || span <= 1u) {
            return 0;
        }
    }
}

uint16_t tcp_bind_ephemeral(void)
{
    uint32_t flags = net_lock();
    uint16_t port = tcp_free_port();

    net_unlock(flags);
    return port;
}

/**
 * Whether a LISTEN block has `port` at an address that meets `ip`: either is
 * any address (0), or both are the same one. The caller holds `net_lock`.
 *
 * A block in another state does not keep a new listener off its local port,
 * and `SO_REUSEADDR` changes nothing here.
 */
static int tcp_port_has_listener(const struct socket *sock, uint16_t port)
{
    const struct tcb *t;

    if (port_fixed[port] == 0) {
        return 0; /* every listener counts its port there */
    }
    for (t = tcbs; t && t != tw_first; t = t->next) {
        if (t->state == TCP_LISTEN && t->local_port == port &&
            (t->sock && socket_bind_conflict(t->sock, sock))) {
            return 1;
        }
    }
    return 0;
}

/** Whether `a` and `b` are one address, their IPv6 scopes compared unless
 * `bound`: a connection bound to a card is on that card's link whatever
 * scope it connected with, as Linux compares the binding, not a scope. */
static int tcp_addr_is(net_addr_t a, net_addr_t b, int bound)
{
    if (bound && a.family == AF_INET6 && b.family == AF_INET6)
        return ip6_equal(a.v6, b.v6);
    return net_addr_equal(a, b);
}

/**
 * Whether a block other than a listener of the network namespace `ns` has
 * this local address and port and remote address and port, one in
 * TIME_WAIT included, the four that `tcp_find` matches. The caller holds
 * `net_lock`.
 *
 * A listener, or a connection from another local address or to another peer,
 * may share the local port: `tcp_find` takes the connection that matches and
 * falls back to a listener only when none does. A block and the new connection,
 * bound to `ifindex` or 0, are on one card unless both name a card, by a
 * binding or else an IPv6 peer's scope, and the cards differ, as Linux compares
 * bindings. Their addresses are compared as `tcp_find` compares them: without
 * scopes when either block is bound to a card, and with them when neither is
 * (`tcp_addr_is`). A peer whose address a card of this machine owns, whatever
 * scope it names, is on no card, since IPv6 routing finds such an address
 * before it reads the scope and its segments come off the loopback backlog,
 * which every binding takes (`tcp_rx_card_ok`).
 */
static int tcp_pair_in_use(const struct netns *ns, net_addr_t local_ip, uint16_t local_port,
                           net_addr_t remote_ip, uint16_t remote_port, int ifindex)
{
    const struct tcb *t;
    uint32_t mine = ifindex ? (uint32_t)ifindex : remote_ip.scope, its;
    net_addr_t bare = remote_ip;
    int bound;

    bare.scope = 0;
    if (net_addr_local(bare) || (remote_ip.family == AF_INET6 ? ip6_is_loopback(remote_ip.v6)
                                                              : IP_IS_LOOPBACK(remote_ip.v4))) {
        mine = 0;
    }
    for (t = tcp_hash[tcp_hash_index(local_port, &remote_ip, remote_port)]; t; t = t->hash_next) {
        if (t->local_port != local_port || t->ns != ns || t->remote_port != remote_port) {
            continue;
        }
        its = t->ifindex ? (uint32_t)t->ifindex : t->remote_ip.scope;
        bound = ifindex || t->ifindex;
        if ((!mine || !its || mine == its) && tcp_addr_is(t->remote_ip, remote_ip, bound) &&
            tcp_addr_is(t->local_ip, local_ip, bound)) {
            return 1;
        }
    }
    return 0;
}

/**
 * Answer a segment that matches no connection with the reset of RFC 793.
 *
 * Without it a SYN to a closed port is lost, and the peer's `connect` waits
 * until its retransmissions give up; over loopback no other host would
 * answer. A segment with an ACK gets a reset at the sequence number it
 * acknowledged, the next the peer expects, and `seg_occupied` is not read;
 * any other gets a reset that acknowledges every sequence number the
 * segment occupied, its SYN and FIN one each, so that the peer accepts it.
 * The callers never pass a reset, since a reset is not answered with one.
 * The same reset answers a SYN to a full backlog or one the machine has no
 * memory for, and a handshake that completes after its listener closed,
 * and a connection's own resets; `ifindex` is the card such a connection or
 * listener is bound to, which the reset leaves by, and 0 for none.
 */
static void tcp_send_rst_unmatched(int ifindex, net_addr_t local_ip, net_addr_t remote_ip,
                                   uint16_t local_port, uint16_t remote_port, uint8_t seg_flags,
                                   uint32_t seg_seq, uint32_t seg_ack, uint32_t seg_occupied)
{
    uint8_t flags;
    uint32_t seq, ack;

    if (seg_flags & TCP_ACK) {
        flags = TCP_RST;
        seq = seg_ack;
        ack = 0;
    } else {
        flags = TCP_RST | TCP_ACK;
        seq = 0;
        ack = seg_seq + seg_occupied;
    }

    write16be(tcp_tx_buf + 0, local_port);
    write16be(tcp_tx_buf + 2, remote_port);
    write32be(tcp_tx_buf + 4, seq);
    write32be(tcp_tx_buf + 8, ack);
    tcp_tx_buf[12] = (uint8_t)((TCP_HDR_SIZE >> 2) << 4);
    tcp_tx_buf[13] = flags;
    write16be(tcp_tx_buf + 14, 0);
    write16be(tcp_tx_buf + 16, 0);
    write16be(tcp_tx_buf + 18, 0);
    write16be(tcp_tx_buf + 16, tcp_checksum(local_ip, remote_ip, tcp_tx_buf, TCP_HDR_SIZE));
    tcp_ip_tx(ip_ns, local_ip, remote_ip, ifindex, TCP_HDR_SIZE);
}

void tcp_ns_gone(const struct netns *ns, const struct netns *to)
{
    struct tcb *t;

    for (t = tcbs; t; t = t->next) {
        if (t->ns == ns) {
            t->ns = to;
        }
    }
}

/**
 * The LISTEN block for a segment to `local_ip`:`local_port` in the network
 * namespace `ip_ns`: the one bound to that address, else the one bound to
 * any, else NULL.
 */
/** Whether a segment that came by the card `ip_rx_ifindex` is for `t`: one
 * bound to a card takes only what came by it, or off the loopback backlog,
 * as Linux's socket lookup compares the binding with the arriving card. */
static int tcp_rx_card_ok(const struct tcb *t)
{
    return t->ifindex == 0 || ip_rx_ifindex == 0 || t->ifindex == ip_rx_ifindex;
}

static struct tcb *tcp_find_listener(net_addr_t local_ip, uint16_t local_port)
{
    struct tcb *t;
    struct tcb *any = NULL;

    for (t = tcbs; t && t != tw_first; t = t->next) {
        if (t->state != TCP_LISTEN || t->local_port != local_port || t->ns != ip_ns ||
            !tcp_rx_card_ok(t))
            continue;
        if (net_addr_equal(t->local_ip, local_ip))
            return t;
        if (t->sock && socket_address_matches(t->sock, local_ip))
            any = t;
    }
    return any;
}

/**
 * Find the TCB for an incoming segment in the network namespace `ip_ns`: a
 * block in any state but LISTEN with the same local address and port,
 * remote address and remote port, from its bucket of `tcp_hash`, else the
 * listener for its local address and port; of those bound to a card, only
 * one the segment came by (`tcp_rx_card_ok`).
 */
static struct tcb *tcp_find(net_addr_t local_ip, uint16_t local_port, net_addr_t remote_ip,
                            uint16_t remote_port)
{
    struct tcb *t;

    for (t = tcp_hash[tcp_hash_index(local_port, &remote_ip, remote_port)]; t; t = t->hash_next) {
        if (t->local_port == local_port && t->ns == ip_ns &&
            tcp_addr_is(t->local_ip, local_ip, t->ifindex) &&
            tcp_addr_is(t->remote_ip, remote_ip, t->ifindex) && t->remote_port == remote_port &&
            tcp_rx_card_ok(t)) {
            return t;
        }
    }
    return tcp_find_listener(local_ip, local_port);
}

/**
 * Send a segment of one byte, the send ring's at the count `at`, at
 * sequence `seq`, without moving `snd_nxt` or arming a timer. Returns what
 * `tcp_ip_tx` returned.
 */
static int tcp_send_byte(struct tcb *tcb, uint32_t seq, uint32_t at)
{
    uint16_t seg_len = TCP_HDR_SIZE + 1;

    write16be(tcp_tx_buf + 0, tcb->local_port);
    write16be(tcp_tx_buf + 2, tcb->remote_port);
    write32be(tcp_tx_buf + 4, seq);
    write32be(tcp_tx_buf + 8, tcb->rcv_nxt);
    tcp_tx_buf[12] = (uint8_t)((TCP_HDR_SIZE >> 2) << 4);
    tcp_tx_buf[13] = TCP_ACK;
    write16be(tcp_tx_buf + 14, tcp_wnd_field(tcb, TCP_ACK));
    write16be(tcp_tx_buf + 16, 0);
    write16be(tcp_tx_buf + 18, 0);
    tcp_tx_buf[TCP_HDR_SIZE] = tcb->send_buf[at & (TCP_RING_SIZE - 1)];

    write16be(tcp_tx_buf + 16, tcp_checksum(tcb->local_ip, tcb->remote_ip, tcp_tx_buf, seg_len));
    return tcp_tcb_tx(tcb, seg_len);
}

/**
 * Send a zero-window probe as Linux's tcp_xmit_probe_skb does: an
 * acknowledgement with no data at `snd_una` less one, a number the peer has
 * had, which it answers with its window whether or not that window would
 * take a byte. A probe that carried a byte at `snd_una` would be taken by a
 * receiver whose free space rounds down to a window of 0, and its answer,
 * past `snd_nxt`, would answer no probe. An empty ring sends nothing.
 * Returns what `tcp_emit_flags` returned, or 0 for nothing sent.
 */
static int tcp_send_zwp(struct tcb *tcb)
{
    if (tcb->send_tail == tcb->send_ack)
        return 0;
    return tcp_emit_flags(tcb, TCP_ACK, tcb->snd_una - 1u);
}

/**
 * Abandon a connection that timed out: close it with ETIMEDOUT, or with the
 * soft error of a send refused for want of a route since the last
 * acknowledgement, as Linux reports `sk_err_soft`, which SO_ERROR and
 * POLLERR report, and wake any blocked call. A passive block in
 * SYN_RCVD, whose handshake the peer never finished, has no descriptor and no
 * place on an accept queue, so it is freed here with its socket; any other
 * block is freed only when it has no socket. Nothing is logged: a timeout is
 * an ordinary event of a network, which the program sees in its error and
 * `nstat` in TcpEstabResets or TcpAttemptFails.
 */
static void tcp_giveup(struct tcb *tcb)
{
    tcp_count_end(tcb->state);
    if (tcb->passive && tcb->state == TCP_SYN_RCVD && tcb->sock) {
        struct socket *sock = tcb->sock;

        tcb->sock = NULL;
        sock->tcb = NULL;
        tcb_free(tcb);
        socket_unref(sock);
        return;
    }
    tcb->state = TCP_CLOSED;
    tcp_delack_clear(tcb);
    tcb->so_error = tcb->soft_error ? tcb->soft_error : ETIMEDOUT;
    tcb->retx_flags = 0;
    tcb->rto_due = 0;
    tcb->zwp_due = 0;
    tcb->recovering = 0;
    tcb->fast_pending = 0;

    if (tcb->sock) {
        socket_tcp_connected(tcb->sock);
        socket_tcp_fin(tcb->sock);
    } else {
        tcb_free(tcb);
    }
}

/* Where the tick's pump rotation continues: the number of connections with
 * queued bytes to pass over before pumping, advanced past the last pumped. */
static uint32_t tcp_pump_next;

/**
 * Send what the tick's pump owes `tcb` (`tcp_send_more`), and give the
 * connection up when the driver has refused its transmits, with nothing sent
 * between, for more than TCP_REFUSE_GIVEUP_TICKS. While the card reports its
 * link down the wait starts again at each tick, so that it counts only the
 * refusals of a link that is up and a cable out for a minute ends nothing.
 * Returns 1 when it gave the connection up, which may have freed `tcb`, and
 * 0 otherwise.
 */
static int tcp_tick_pump(struct tcb *tcb)
{
    if (tcp_send_more(tcb) != 1 || tcb->refused_since == 0) {
        return 0;
    }
    if (net_addr_link_down(tcb->remote_ip, tcb->ifindex)) {
        tcb->refused_since = TCP_NOW() + 1;
        return 0;
    }
    if (TCP_NOW() + 1 - tcb->refused_since > TCP_REFUSE_GIVEUP_TICKS) {
        klog(KLOG_WARN, "tcp", "gave up: the driver refused every transmit for %d ms",
             (int)(tick_msecs((uint32_t)(TCP_NOW() + 1 - tcb->refused_since))));
        tcp_giveup(tcb);
        return 1;
    }
    return 0;
}

/**
 * Whether the tick's pump has anything to do for a connection: queued bytes
 * in a state that sends them, ESTABLISHED or CLOSE_WAIT; or, in any state
 * after the handshake but TIME_WAIT, a fast recovery's refused resend or a
 * timeout's recovery with data still to send again, since a close with a
 * lost segment in flight would otherwise wait a timeout per segment.
 */
static int tcp_pumpable(const struct tcb *tcb)
{
    if (tcb->state == TCP_CLOSED || tcb->state == TCP_LISTEN || tcb->state == TCP_TIME_WAIT ||
        tcb->state == TCP_SYN_SENT || tcb->state == TCP_SYN_RCVD) {
        return 0;
    }
    if (tcb->fast_pending) {
        return 1;
    }
    if (tcb->recovering == TCP_RECOVER_RTO && (int32_t)(tcb->retx_nxt - tcb->recover) < 0) {
        return 1;
    }
    return (tcb->state == TCP_ESTABLISHED || tcb->state == TCP_CLOSE_WAIT) &&
           tcb->send_tail != tcb->send_head;
}

/**
 * Whether `tcb` is a detached block that still has data or a FIN to deliver,
 * in ESTABLISHED, CLOSE_WAIT, FIN_WAIT_1, CLOSING or LAST_ACK, whose peer has
 * acknowledged nothing since its limit (`timewait_until`) was set, at the
 * close or at the last acknowledgement of new data. No program waits on such
 * a block, so a peer that has gone, or a path that drops its segments, would
 * otherwise hold it for the minutes TCP_MAX_RETX doublings of the timeout
 * take.
 */
static int tcp_orphan_expired(const struct tcb *tcb)
{
    if (tcb->sock) {
        return 0;
    }
    if (tcb->state != TCP_ESTABLISHED && tcb->state != TCP_CLOSE_WAIT &&
        tcb->state != TCP_FIN_WAIT_1 && tcb->state != TCP_CLOSING && tcb->state != TCP_LAST_ACK) {
        return 0;
    }
    return (int32_t)(TCP_NOW() - tcb->timewait_until) >= 0;
}

/**
 * The tick's pumps, in rotation: up to TCP_TICK_PUMPS connections with
 * queued bytes, starting after the last one pumped, so that a connection
 * behind others that a zero window holds is reached. The connections are
 * numbered in list order among those with queued bytes; the ones at and
 * after the rotation's point go first, then the ones before it.
 */
static void tcp_tick_pumps(void)
{
    struct tcb *tcb, *next;
    uint32_t start = tcp_pump_next, eligible = 0, pumped = 0, index = 0;

    for (tcb = tcbs; tcb && tcb != tw_first; tcb = next) {
        next = tcb->next;
        if (!tcp_pumpable(tcb)) {
            continue;
        }
        if (index >= start && pumped < TCP_TICK_PUMPS) {
            pumped++;
            tcp_pump_next = index + 1;
            (void)tcp_tick_pump(tcb);
        }
        index++;
        eligible++;
    }
    index = 0;
    for (tcb = tcbs; tcb && tcb != tw_first && pumped < TCP_TICK_PUMPS && index < start;
         tcb = next) {
        next = tcb->next;
        if (!tcp_pumpable(tcb)) {
            continue;
        }
        pumped++;
        tcp_pump_next = index + 1;
        (void)tcp_tick_pump(tcb);
        index++;
    }
    if (tcp_pump_next >= eligible) {
        tcp_pump_next = 0;
    }
}

/**
 * The ticks Linux's retransmissions take before a synchronized connection
 * is given up: the sum of `tcp_retries2` + 1 timeouts, the first
 * TCP_MODEL_RTO_BASE_MS and each twice the one before, up to
 * TCP_MODEL_RTO_MAX_MS, as Linux's `tcp_model_timeout` computes it; 924.6 s
 * for the default of 15.
 */
static uint32_t tcp_retx_limit(void)
{
    uint32_t ms = 0, rto = TCP_MODEL_RTO_BASE_MS;
    int i;

    for (i = 0; i <= tcp_retries2; i++) {
        ms += rto;
        rto = rto >= TCP_MODEL_RTO_MAX_MS / 2u ? TCP_MODEL_RTO_MAX_MS : rto * 2u;
    }
    return tcp_ms_ticks(ms);
}

/**
 * Whether the timeouts in a row of `tcb` have run past its limit, counted
 * from `retx_stamp`: the socket's TCP_USER_TIMEOUT when it set one, in any
 * state; else TCP_SYN_RETRIES retransmissions in the handshake; else the
 * time of `tcp_retx_limit`.
 */
static int tcp_retx_expired(const struct tcb *tcb)
{
    uint32_t elapsed = TCP_NOW() + 1 - tcb->retx_stamp;

    if (tcb->sock && tcb->sock->user_timeout) {
        return elapsed >= tcp_ms_ticks(tcb->sock->user_timeout);
    }
    if (tcb->state == TCP_SYN_SENT || tcb->state == TCP_SYN_RCVD) {
        return tcb->rto_backoff > TCP_SYN_RETRIES;
    }
    return elapsed >= tcp_retx_limit();
}

/** Shorten the retransmission timer to end no later than the socket's
 * TCP_USER_TIMEOUT, so that the give-up comes when it asks, as Linux's
 * `tcp_clamp_rto_to_user_timeout` does. */
static void tcp_user_timeout_clamp(struct tcb *tcb)
{
    uint32_t limit, elapsed;

    if (!tcb->sock || !tcb->sock->user_timeout || tcb->retx_stamp == 0 || tcb->rto_due == 0) {
        return;
    }
    limit = tcp_ms_ticks(tcb->sock->user_timeout);
    elapsed = TCP_NOW() + 1 - tcb->retx_stamp;
    if (elapsed >= limit) {
        tcb->rto_due = tcp_due(1);
    } else if (tcp_left(tcb->rto_due) > limit - elapsed) {
        tcb->rto_due = tcp_due(limit - elapsed);
    }
}

/**
 * The wait `when` before the next zero-window probe, shortened to end no
 * later than TCP_USER_TIMEOUT allows from the first probe, though at least
 * a tick, as Linux's tcp_clamp_probe0_to_user_timeout does.
 */
static uint32_t tcp_zwp_clamp(const struct tcb *tcb, uint32_t when)
{
    uint32_t limit, elapsed;

    if (!tcb->sock || !tcb->sock->user_timeout || tcb->zwp_stamp == 0) {
        return when;
    }
    limit = tcp_ms_ticks(tcb->sock->user_timeout);
    elapsed = TCP_NOW() + 1 - tcb->zwp_stamp;
    if (elapsed >= limit) {
        return 1;
    }
    return when < limit - elapsed ? when : limit - elapsed;
}

/**
 * The wait before the next zero-window probe, as Linux's tcp_probe0_when:
 * the timeout, at least TCP_RTO_MIN, doubled `zwp_backoff` times, up to
 * TCP_PROBE_MAX.
 */
static uint32_t tcp_zwp_when(const struct tcb *tcb)
{
    uint32_t when = tcb->rto > TCP_RTO_MIN ? tcb->rto : TCP_RTO_MIN;
    uint32_t i;

    for (i = 0; i < tcb->zwp_backoff && when < TCP_PROBE_MAX; i++) {
        when <<= 1;
    }
    return when < TCP_PROBE_MAX ? when : TCP_PROBE_MAX;
}

/**
 * Whether the retransmission timer of `tcb` probes a closed window rather
 * than counts a timeout, as Linux's tcp_retransmit_timer does: the peer's
 * window is 0 over data or a FIN in flight, in a synchronized state, and a
 * socket holds the block. A detached block is
 * given up as any other (`tcp_orphan_expired`).
 */
static int tcp_zwp_in_flight(const struct tcb *tcb)
{
    return tcb->snd_wnd == 0 && tcb->sock && tcb->state >= TCP_ESTABLISHED &&
           tcb->snd_una != tcb->snd_nxt;
}

/**
 * Whether the probing of a closed window over data in flight, which the
 * retransmission timer due at `due` does, has run out, as Linux's
 * tcp_rtx_probe0_timed_out says: with TCP_USER_TIMEOUT set, once that long
 * has passed since `retx_stamp`, the loss's first retransmission or the
 * first probe's tick, or at a first probe that no retransmission came
 * before, the oldest data's send (`tcp_rto_expire`); otherwise only once
 * no acceptable acknowledgement (`rcv_stamp`) was received for twice
 * TCP_PROBE_MAX before the timer's deadline and the probes have lasted that
 * long, or the user timeout if shorter. A peer that answers the probes
 * keeps the connection.
 */
static int tcp_zwp_rtx_expired(const struct tcb *tcb, uint32_t due)
{
    uint32_t limit = 2u * TCP_PROBE_MAX, rtx = TCP_NOW() + 1 - tcb->retx_stamp, user;

    if (tcb->sock->user_timeout) {
        user = tcp_ms_ticks(tcb->sock->user_timeout);
        if (rtx > user) {
            return 1;
        }
        if (user < limit) {
            limit = user;
        }
    }
    if ((int32_t)(due - tcb->rcv_stamp) <= (int32_t)limit) {
        return 0;
    }
    return rtx > limit;
}

/**
 * Run the zero-window probe timer of `tcb`, which has expired, as Linux's
 * tcp_probe_timer and tcp_send_probe0 do. With the window open or nothing
 * left to send, nothing goes. With bytes in flight, the retransmission
 * timer probes (`tcp_zwp_in_flight`), and this timer stops until an
 * acknowledgement of them all starts it again. Otherwise each probe
 * (`tcp_send_zwp`) counts in `zwp_probes`,
 * which any acknowledgement clears (`tcp_rx_addr`), and doubles the wait
 * before the next (`tcp_zwp_when`), or waits TCP_RESOURCE_PROBE_TICKS when
 * the driver refused it, each wait clamped by `tcp_zwp_clamp`. The
 * connection is given up with ETIMEDOUT, counted in TCPAbortOnTimeout, as
 * Linux's tcp_write_err gives it up, once `tcp_retries2` probes have gone
 * unanswered, or with TCP_USER_TIMEOUT set, once that long has passed since
 * the first probe while the window stayed closed, answered or not. A
 * detached block is given up sooner by `tcp_orphan_expired`. Returns 1
 * when the connection was given up, and `tcb` may be gone.
 */
static int tcp_zwp_expire(struct tcb *tcb)
{
    uint32_t now = TCP_NOW(), when;
    int expired = 0, rc;

    tcb->zwp_due = 0;
    if (tcb->snd_wnd != 0 || tcb->send_tail == tcb->send_ack) {
        tcb->zwp_probes = 0;
        tcb->zwp_stamp = 0;
        return 0;
    }
    if (tcb->snd_una != tcb->snd_nxt) {
        tcb->zwp_probes = 0;
        tcb->zwp_stamp = 0;
        return 0;
    }
    if (tcb->zwp_stamp == 0) {
        tcb->zwp_stamp = tcp_due(1); /* now + 1, never 0 */
    } else if (tcb->sock && tcb->sock->user_timeout) {
        expired = now + 1 - tcb->zwp_stamp >= tcp_ms_ticks(tcb->sock->user_timeout);
    }
    if (expired || (int)tcb->zwp_probes >= tcp_retries2) {
        net_mib.tcp_abort_on_timeout++;
        tcp_giveup(tcb);
        return 1;
    }
    rc = tcp_send_zwp(tcb);
    if (tcb->zwp_probes < 255) {
        tcb->zwp_probes++;
    }
    if (rc == -ENOBUFS) {
        when = TCP_RESOURCE_PROBE_TICKS;
    } else {
        if ((int)tcb->zwp_backoff < tcp_retries2 && tcb->zwp_backoff < 255) {
            tcb->zwp_backoff++;
        }
        when = tcp_zwp_when(tcb);
    }
    tcb->zwp_due = tcp_due(tcp_zwp_clamp(tcb, when));
    return 0;
}

/**
 * Run the retransmission timer of `tcb`, which has expired. The give-up
 * counts from `retx_stamp`, as Linux's from retrans_stamp. In a
 * synchronized state that is the loss's first retransmission, which a fast
 * retransmission may have made before the timeout (`tcp_resend`), or else
 * this timeout's tick; and the first timeout since an acknowledgement of
 * new data never gives up, as Linux's retransmits_timed_out waits for
 * icsk_retransmits. For the first probe of a closed window when no
 * retransmission came before it, it is the first send of the oldest data
 * in flight (`tcp_sent`), as Linux's tcp_retransmit_timer takes
 * retrans_stamp or else the head segment's stamp, and the first probe then
 * stamps its own tick. In the handshake it is the timer's last arming, the
 * timeout before `due`, which is the send of the SYN or SYN-ACK. The
 * connection is given up when `tcp_retx_expired` says so,
 * and otherwise the timeout doubles, up to TCP_RTO_MAX, and no further
 * than TCP_USER_TIMEOUT allows. Data sent and unacknowledged may lower the
 * segment size (`tcp_blackhole`) and starts a timeout's recovery from the
 * oldest byte, even with a FIN sent behind it, since otherwise a lost data
 * segment is never sent again and the close stalls; the FIN's own
 * retransmission resumes once the data is acknowledged (`send_ack` reaches
 * `send_head`). With no data outstanding, the control segment (SYN, SYN-ACK
 * or FIN) is sent again. A peer's window closed over data in flight makes
 * the timeout a probe instead (`tcp_zwp_in_flight`), as on Linux: it is not
 * counted in TCPTimeouts, lowers no segment size, and gives the connection
 * up only as `tcp_zwp_rtx_expired` says. The recovery it starts, which the
 * closed window holds back, sends up to a segment from `snd_una` past the
 * window all the same, counted in TCPRetransSegs and in flight, as Linux's
 * __tcp_retransmit_skb lets the oldest segment past a window of 0: a peer
 * whose window has opened takes it, and one whose window is still closed
 * drops it and answers with its window. Returns 1 when the connection was
 * given up, and `tcb` may be gone.
 */
static int tcp_rto_expire(struct tcb *tcb)
{
    uint32_t due = tcb->rto_due;
    int zwp = tcp_zwp_in_flight(tcb), sync = tcb->state >= TCP_ESTABLISHED;
    int first = tcb->rto_backoff == 0, probe0 = 0, n;

    tcb->rto_due = 0;
    if (tcb->retx_stamp == 0) {
        if (sync && !zwp) {
            tcb->retx_stamp = tcp_due(1); /* this tick + 1 */
        } else if (zwp && tcb->sent_runs > 0) {
            tcb->retx_stamp = tcp_sent(tcb)[0].tick;
            probe0 = 1;
        } else {
            tcb->retx_stamp = due + 1 - tcb->rto; /* the arming's tick + 1 */
            if (tcb->retx_stamp == 0)
                tcb->retx_stamp = 1;
            probe0 = zwp;
        }
    }
    if (tcb->rto_backoff < 255)
        tcb->rto_backoff++;
    if (zwp ? tcp_zwp_rtx_expired(tcb, due) : (!sync || !first) && tcp_retx_expired(tcb)) {
        if (sync)
            net_mib.tcp_abort_on_timeout++;
        tcp_giveup(tcb);
        return 1;
    }
    if (probe0)
        tcb->retx_stamp = tcp_due(1); /* the first probe's tick + 1 */
    if (!zwp)
        net_mib.tcp_timeouts++;

    tcb->rto <<= 1;
    if (tcb->rto > TCP_RTO_MAX)
        tcb->rto = TCP_RTO_MAX;

    if (tcb->send_head != tcb->send_ack) {
        if (!zwp)
            tcp_blackhole(tcb);
        tcp_enter_rto_recovery(tcb);
        if (zwp) {
            n = tcp_resend(tcb, tcb->snd_una, tcb->snd_mss);
            if (n > 0)
                tcb->retx_nxt += (uint32_t)n;
        }
    } else if (tcb->retx_flags) {
        net_mib.tcp_retrans_segs += tcp_emit_flags(tcb, tcb->retx_flags, tcb->retx_seq) == 0;
        tcb->retransmits++;
        tcb->rto_due = tcp_due(tcb->rto);
    }
    tcp_user_timeout_clamp(tcb);
    return 0;
}

/** The ticks of a keepalive time or interval of `secs` seconds, none for
 * 0 or less: /proc/sys/net/ipv4's tcp_keepalive_time and _intvl take
 * either sign, as Linux's `proc_dointvec_jiffies` does, and Linux's timer
 * set that far takes a time already past as due. */
static uint32_t tcp_keep_ticks(int secs)
{
    return secs > 0 ? (uint32_t)secs * TICK_HZ : 0u;
}

/**
 * Run the keepalive of `tcb` for a tick (TCP_KEEPIDLE_DEFAULT). It is armed
 * from the tick that finds SO_KEEPALIVE set on the socket, which counts as
 * a segment received, and disarmed when the option is cleared. In
 * ESTABLISHED, CLOSE_WAIT or FIN_WAIT_2, with nothing queued or in flight,
 * a probe goes when the idle time has passed since `ka_since`, the last
 * acceptable acknowledgement or in-order data received, and another each
 * interval after it, as Linux's tcp_keepalive_timer counts from the later
 * of rcv_tstamp and lrcvtime. Only an acceptable acknowledgement answers
 * the probes (`tcp_rx_answers`); data alone moves `ka_since`, so that the
 * count goes on from where it was once the idle time has passed again.
 * When the count of probes went unanswered, or with TCP_USER_TIMEOUT set,
 * when a probe is out and that long has passed since `ka_since`, the peer
 * is sent a reset and the connection is given up with ETIMEDOUT, as Linux
 * gives it up. The socket's values and the defaults are read at each tick,
 * so a new one applies at once, measured from `ka_since`.
 *
 * The probe is one byte at `snd_una` less one, which the peer has had and
 * answers with an acknowledgement of `snd_una` (RFC 1122, 4.2.3.6): the
 * last byte acknowledged, which the empty ring still holds. Each counts in
 * TCPKeepAlive, sent or refused.
 */
static void tcp_keepalive(struct tcb *tcb)
{
    struct socket *s = tcb->sock;
    uint32_t now = TCP_NOW(), idle, intvl, cnt;

    if (!s || !s->keepalive) {
        tcb->ka_armed = 0;
        return;
    }
    if (!tcb->ka_armed) {
        tcb->ka_armed = 1;
        tcb->ka_since = now;
        tcb->ka_probes = 0;
        return;
    }
    if ((tcb->state != TCP_ESTABLISHED && tcb->state != TCP_CLOSE_WAIT &&
         tcb->state != TCP_FIN_WAIT_2) ||
        tcb->snd_una != tcb->snd_nxt || tcb->send_tail != tcb->send_head) {
        return;
    }
    idle = tcp_keep_ticks(s->keepidle ? s->keepidle : tcp_keepalive_time);
    intvl = tcp_keep_ticks(s->keepintvl ? s->keepintvl : tcp_keepalive_intvl);
    cnt = (uint32_t)(s->keepcnt ? s->keepcnt : tcp_keepalive_probes);
    if (now - tcb->ka_since < idle || (tcb->ka_probes > 0 && now - tcb->ka_probe_at < intvl)) {
        return;
    }
    if (s->user_timeout ? tcb->ka_probes > 0 && now - tcb->ka_since >= tcp_ms_ticks(s->user_timeout)
                        : tcb->ka_probes >= cnt) {
        (void)tcp_emit_flags(tcb, TCP_RST | TCP_ACK, tcb->snd_nxt);
        tcp_giveup(tcb);
        return;
    }
    (void)tcp_send_byte(tcb, tcb->snd_una - 1u, tcb->send_ack - 1u);
    net_mib.tcp_keepalive++;
    if (tcb->ka_probes < 255) {
        tcb->ka_probes++;
    }
    tcb->ka_probe_at = now;
}

/**
 * The tick's work on the TIME_WAIT run: free the blocks at its head whose
 * time is up, and, while one owes an acknowledgement the card refused
 * (`tw_owed`), send each owed one again. The rest of the run is not read.
 */
static void tcp_tick_time_wait(void)
{
    struct tcb *tcb, *next;

    while (tw_first && (int32_t)(TCP_NOW() - tw_first->timewait_until) >= 0) {
        tcb_free(tw_first);
    }
    if (!tw_owed) {
        return;
    }
    tw_owed = 0;
    for (tcb = tw_first; tcb; tcb = next) {
        next = tcb->next;
        if (tcb->ack_owed) {
            tcb->ack_owed = 0;
            tcp_send_flags(tcb, TCP_ACK);
        }
    }
}

/**
 * Send the acknowledgement `tcb` held, whose deadline has come, with the
 * window as it stands now, in any state that has something to
 * acknowledge. A deadline that comes in ping-pong mode means that the
 * answer the hold waited for did not come, and ends the mode, as Linux's
 * tcp_delack_timer_handler does.
 */
static void tcp_delack_expire(struct tcb *tcb)
{
    tcb->delack_due = 0;
    if (tcb->state != TCP_CLOSED && tcb->state != TCP_LISTEN && tcb->state != TCP_SYN_SENT) {
        tcb->pingpong = 0;
        tcb->rcv_wnd = tcp_rcv_window(tcb);
        tcp_send_flags(tcb, TCP_ACK);
    }
}

/**
 * The FIN_WAIT_2 limit, TCP_FIN_WAIT2_TICKS, frees a detached block whose
 * peer went away with neither a FIN nor a reset, and which would otherwise
 * never be freed (cf. Linux tcp_fin_timeout). Any other detached block that
 * still sends is given up once its peer has acknowledged nothing for
 * TCP_ORPHAN_TICKS (`tcp_orphan_expired`).
 *
 * An acknowledgement the interface refused (`ack_owed`) is sent again as a
 * fresh one, carrying the receive state as it now stands, in any state but
 * CLOSED, LISTEN and SYN_SENT, which have nothing to acknowledge. TIME_WAIT
 * is included: a connection enters it by acknowledging the peer's FIN, and a
 * refused acknowledgement would otherwise wait for the peer to send its FIN
 * again, which the block may not outlive.
 *
 * The retransmission and zero-window timers are deadlines on the tick count
 * (`tcp_due`), so a call that follows ticks the boot processor's tick passed
 * over while another processor held the big lock still fires them on time.
 *
 * The retransmission timer runs as `tcp_rto_expire` says. The zero-window
 * probe timer, with nothing in flight, sends a probe while the peer's
 * window stays closed, backs off and gives up as `tcp_zwp_expire` says.
 * Then the keepalive runs (`tcp_keepalive`). The blocks of the TIME_WAIT
 * run are left to `tcp_tick_time_wait`. Last, `tcp_tick_pumps` sends what
 * refused transmits left in the rings. Each block's successor is read
 * before the block can be freed.
 */
void tcp_tick(void)
{
    struct tcb *tcb, *next;

    for (tcb = tcbs; tcb && tcb != tw_first; tcb = next) {
        next = tcb->next;

        if (tcb->state == TCP_TIME_WAIT) {
            if ((int32_t)(TCP_NOW() - tcb->timewait_until) >= 0 && !tcb->sock) {
                tcb_free(tcb);
                continue;
            }
            if (tcb->ack_owed) {
                tcb->ack_owed = 0;
                tcp_send_flags(tcb, TCP_ACK);
            }
            continue;
        }

        if (tcb->ack_owed && tcb->state != TCP_CLOSED && tcb->state != TCP_LISTEN &&
            tcb->state != TCP_SYN_SENT) {
            tcb->ack_owed = 0;
            tcp_send_flags(tcb, TCP_ACK);
        } else if (tcb->ack_owed) {
            tcb->ack_owed = 0; /* nothing to acknowledge, as a reset left it */
        }

        if (tcb->delack_due != 0 && tcp_expired(tcb->delack_due)) {
            tcp_delack_expire(tcb);
        }

        if (tcb->state == TCP_FIN_WAIT_2) {
            if ((int32_t)(TCP_NOW() - tcb->timewait_until) >= 0 && !tcb->sock) {
                tcb_free(tcb);
                continue;
            }
            tcp_keepalive(tcb);
            continue;
        }

        if (tcp_orphan_expired(tcb)) {
            tcp_giveup(tcb);
            continue;
        }

        if (tcb->rto_due != 0 && tcp_expired(tcb->rto_due) && tcp_rto_expire(tcb)) {
            continue;
        }

        if (tcb->zwp_due != 0 && tcp_expired(tcb->zwp_due) && tcp_zwp_expire(tcb)) {
            continue;
        }

        tcp_keepalive(tcb);
    }
    tcp_tick_time_wait();
    tcp_tick_pumps();
}

/**
 * `bind` reports the collision where a server expects to hear of it. The
 * check is advisory: `tcp_listen` is what keeps one listener to a port.
 */
int tcp_port_listening(const struct socket *sock, uint16_t port)
{
    uint32_t flags = net_lock();
    int held = tcp_port_has_listener(sock, port);

    net_unlock(flags);

    return held;
}

/* The first connection a full backlog refused is recorded, so a log that
 * shows a reset before a session says why; the rest are not, since a
 * listener that falls behind would fill the log. */
static int backlog_refusal_logged;

/**
 * The port is checked here and not in `bind`, because `bind` and `listen` are
 * separate system calls with any user code between them: `bind(A, P)`,
 * `bind(B, P)`, `listen(A)`, `listen(B)` passes any check that only `bind`
 * makes, and leaves two LISTEN blocks on one port for `tcp_find` and
 * the passive handshake to disagree about. So the search and the new block
 * are one `net_lock` region, and the check `bind` still makes is advisory.
 *
 * A socket that never bound a port listens on the next ephemeral one, as a
 * `listen` on Linux binds it, and `getsockname` reports the port chosen.
 * A port the tables count no holder of (`tcp_port_in_use`) has no listener
 * and no bound socket, so the search for one is not made, as Linux's bind
 * table answers for a port without a bucket.
 */
int tcp_listen(struct socket *sock, uint16_t port)
{
    struct tcb *tcb;
    uint32_t flags;

    flags = net_lock();
    tcp_isn_key();

    if (port == 0) {
        port = tcp_free_port();
    }
    if (port == 0 || (tcp_port_in_use(port) &&
                      (tcp_port_has_listener(sock, port) || socket_stream_conflict(sock, port)))) {
        net_unlock(flags);
        return -EADDRINUSE;
    }

    tcb = tcb_alloc();
    if (!tcb) {
        net_unlock(flags);
        return -ENOBUFS;
    }

    tcb->family = sock->family;
    tcb->ns = sock->ns;
    tcb->local_ip = sock->local_ip;
    tcb->ifindex = sock->bound_device;
    tcb_take_port(tcb, port, TCP_PORT_FIXED);
    tcb->state = TCP_LISTEN;
    tcb->sock = sock;
    tcb->uid = sock->uid;
    tcb->inode = sock->inode;
    sock->tcb = tcb;
    sock->local_port = port;

    net_unlock(flags);
    return 0;
}

/**
 * The port is settled, the block allocated and the SYN sent in one
 * `net_lock` region, and the port is settled before the block exists, so
 * that no search finds the caller's own half-built block.
 *
 * A socket that bound a port keeps it. What is refused is a local address
 * and port already paired with this same peer address and port
 * (`tcp_pair_in_use`),
 * since two such blocks would have one connection's segments delivered to
 * the other.
 *
 * A socket without a port takes one `tcp_connect_port` chooses, which no
 * listener holds: over loopback, an ephemeral port equal to that of the
 * listener being dialed would give the looped SYN the same source and
 * destination port, match the caller's own block, and hang the handshake.
 *
 * The connection is opened from the address the socket bound, or else from
 * the one `ip_source` gives: 127.0.0.1 to 127/8, and otherwise the address of
 * the interface the route to the peer leaves by, the only address the answer
 * can come back to. An address in 127/8 to a peer that is not this machine
 * is refused, since no card may send from one. A socket bound to a card with
 * SO_BINDTODEVICE takes its source, route and MTU from that card's routes
 * alone (`netdev_route_on`), and its segments leave by that card; an IPv6
 * peer whose scope names another card is -EINVAL, as on Linux.
 */
int tcp_connect(struct socket *sock, net_addr_t dst_ip, uint16_t dst_port)
{
    struct tcb *tcb;
    uint32_t flags;
    uint16_t local;
    net_addr_t from;
    int rc;

    if (dst_ip.family == AF_INET6 && dst_ip.scope && sock->bound_device &&
        dst_ip.scope != (uint32_t)sock->bound_device) {
        return -EINVAL; /* a link's scope other than the card bound to */
    }
    flags = net_lock();
    rc = net_addr_source(sock->local_ip, dst_ip, sock->bound_device, &from);
    if (rc < 0) {
        net_unlock(flags);
        return rc;
    }
    tcp_isn_key();

    if (sock->local_port != 0) {
        local = sock->local_port;
        if (tcp_pair_in_use(sock->ns, from, local, dst_ip, dst_port, sock->bound_device)) {
            net_unlock(flags);
            return -EADDRINUSE;
        }
    } else {
        local = tcp_connect_port(sock->ns, from, dst_ip, dst_port, sock->bound_device);
        if (local == 0) {
            net_unlock(flags);
            return -EADDRNOTAVAIL;
        }
    }

    tcb = tcb_alloc();
    if (tcb && tcp_ring_alloc(tcb) != 0) {
        tcb_free(tcb);
        tcb = 0;
    }
    if (!tcb) {
        net_unlock(flags);
        return -ENOBUFS;
    }

    tcb->family = sock->family;
    tcb->ns = sock->ns;
    tcb->local_ip = from;
    tcb->port_auto = sock->local_port == 0;
    tcb_take_port(tcb, local, tcb->port_auto ? TCP_PORT_AUTO : TCP_PORT_FIXED);
    tcb->remote_ip = dst_ip;
    tcb->remote_port = dst_port;
    tcb->ifindex = sock->bound_device;
    tcb_hash_add(tcb);
    tcb->iss = tcp_isn(tcb);
    tcb->snd_nxt = tcb->iss;
    tcb->snd_una = tcb->iss;
    tcb->recover = tcb->iss;
    tcb->sock = sock;
    tcb->uid = sock->uid;
    tcb->inode = sock->inode;
    tcb->pingpong = sock->quickack_off;

    sock->tcb = tcb;
    sock->local_port = tcb->local_port;
    sock->remote_ip = dst_ip;
    sock->remote_port = dst_port;

    tcp_send_flags(tcb, TCP_SYN);
    tcb->state = TCP_SYN_SENT;
    net_mib.tcp_active_opens++;

    net_unlock(flags);
    return 0;
}

/**
 * The peer's FIN, taken in a FIN_WAIT state: a socket the connection's
 * shutdown left attached reads the end of input after the data before it.
 */
static void tcp_take_fin(struct tcb *tcb)
{
    tcb->fin_received = 1;
    if (tcb->sock) {
        socket_tcp_fin(tcb->sock);
    }
}

/**
 * The end of a closed connection: free the block, or, while a socket that
 * shut its sending half holds it, keep it as CLOSED with the data its reader
 * has yet to read, for the socket's close to free.
 */
static void tcp_closed_or_free(struct tcb *tcb)
{
    if (!tcb->sock) {
        tcb_free(tcb);
        return;
    }
    tcb->state = TCP_CLOSED;
    tcp_delack_clear(tcb);
    tcb->retx_flags = 0;
    tcb->rto_due = 0;
    tcb->zwp_due = 0;
}

/**
 * Disconnect `sock`, whose connection is in SYN_SENT, as Linux's
 * inet_shutdown has tcp_disconnect do for a shutdown in SYN_SENT
 * (net/ipv4/af_inet.c:934-937, net/ipv4/tcp.c:3298-3318): the attempt ends
 * with no segment sent and no count, the socket records ECONNRESET for its
 * `connect` or SO_ERROR, its shutdown flags are cleared, and it may
 * connect again. A port `tcp_connect` chose is given back, as Linux's move
 * to TCP_CLOSE puts a port the socket did not bind. With `net_lock` held.
 */
static void tcp_disconnect(struct socket *sock)
{
    struct tcb *tcb = sock->tcb;

    if (tcb->port_auto) {
        sock->local_port = 0;
    }
    sock->tcb = NULL;
    tcb->sock = NULL;
    tcb_free(tcb);
    sock->so_error = ECONNRESET;
    sock->shutdown = 0;
}

/**
 * shutdown(2) judges the state as Linux's inet_shutdown does
 * (net/ipv4/af_inet.c:915-937 in 6.12.115). A listener shut for receiving
 * stops listening, as tcp_disconnect has inet_csk_listen_stop stop it
 * (net/ipv4/tcp.c:3302-3303): its block and its half-open connections are
 * freed, each queued connection is reset (`socket_tcp_forget_queue`), the
 * socket's accept is woken to fail with EINVAL, and the socket may listen
 * again; SHUT_WR alone leaves a listener as it is. A connection in SYN_SENT
 * is disconnected (`tcp_disconnect`). A socket with no block, never
 * connected or disconnected, is Linux's TCP_CLOSE, and so is a block that is
 * CLOSED, or in TIME_WAIT with its socket attached, which Linux's full
 * socket has left for TCP_CLOSE already (tcp_time_wait calls tcp_done,
 * net/ipv4/tcp_minisocks.c:364): the flags are set and the call is
 * ENOTCONN, as Linux's TCP_CLOSE case falls through to set them, and a
 * later read and poll honour them (`socket_tcp_recv`, `socket_poll`). They
 * stay set if the socket then connects, as Linux's connect leaves
 * sk_shutdown as it is. Any other
 * state takes the flags, and a send shutdown from ESTABLISHED or CLOSE_WAIT
 * queues the FIN after the data, as tcp_shutdown sends it from those states
 * (net/ipv4/tcp.c:3006-3012); SYN_RCVD, a simultaneous open's, sends none,
 * and a later send there fails with EPIPE once the handshake ends
 * (`tcp_send`). Linux's SYN_SENT is in tcp_shutdown's mask too, but
 * inet_shutdown never reaches it from there.
 */
int tcp_shutdown(struct socket *sock, int rd, int wr)
{
    struct tcb *tcb;
    uint32_t flags = net_lock();
    int rc = 0;

    tcb = sock->tcb;
    if (sock->listening) {
        if (rd) {
            tcp_close(sock);
            socket_tcp_forget_queue(sock);
            sock->listening = 0;
        }
        net_unlock(flags);
        return 0;
    }
    if (tcb && tcb->state == TCP_SYN_SENT) {
        tcp_disconnect(sock);
        net_unlock(flags);
        return 0;
    }
    if (!tcb || tcb->state == TCP_CLOSED || tcb->state == TCP_TIME_WAIT) {
        rc = -ENOTCONN;
    }
    if (rd) {
        sock->shutdown |= SOCK_RCV_SHUTDOWN;
    }
    if (wr) {
        sock->shutdown |= SOCK_SEND_SHUTDOWN;
        if (tcb && (tcb->state == TCP_ESTABLISHED || tcb->state == TCP_CLOSE_WAIT)) {
            tcb->fin_pending = 1;
            tcp_pump(tcb);
        }
    }
    net_unlock(flags);
    return rc;
}

/**
 * Linux's tcp_disconnect, which inet_child_forget calls for each queued
 * connection (net/ipv4/inet_connection_sock.c:1388-1391), sends
 * tcp_send_active_reset from the states tcp_need_reset names, ESTABLISHED,
 * CLOSE_WAIT, FIN_WAIT_1, FIN_WAIT_2 and SYN_RCVD (net/ipv4/tcp.c:3249-3254,
 * 3306-3308), and its move to TCP_CLOSE counts EstabResets from
 * ESTABLISHED and CLOSE_WAIT. CLOSING and LAST_ACK, which follow a FIN of
 * this side, are not reached by a connection nobody accepted.
 */
void tcp_forget_queued(struct socket *sock)
{
    struct tcb *tcb;
    uint32_t flags = net_lock();

    tcb = sock->tcb;
    if (!tcb) {
        net_unlock(flags);
        return;
    }
    sock->tcb = NULL;
    tcb->sock = NULL;
    switch (tcb->state) {
    case TCP_ESTABLISHED:
    case TCP_CLOSE_WAIT:
        net_mib.tcp_estab_resets++;
        (void)tcp_emit_flags(tcb, TCP_RST | TCP_ACK, tcb->snd_nxt);
        break;
    case TCP_FIN_WAIT_1:
    case TCP_FIN_WAIT_2:
    case TCP_SYN_RCVD:
        (void)tcp_emit_flags(tcb, TCP_RST | TCP_ACK, tcb->snd_nxt);
        break;
    case TCP_CLOSED:
    case TCP_LISTEN:
    case TCP_SYN_SENT:
    case TCP_CLOSING:
    case TCP_LAST_ACK:
    case TCP_TIME_WAIT:
    default:
        break;
    }
    tcb_free(tcb);
    net_unlock(flags);
}

/**
 * Give back the receive ring of a block in TIME_WAIT once no program can
 * read from it: no socket holds the block, or the ring has nothing unread.
 * TIME_WAIT takes no more data, so the ring is never filled again, and a
 * block there keeps only itself, as Linux's TIME_WAIT keeps no buffers; a
 * burst of short connections would otherwise hold a ring each for
 * TCP_2MSL_TICKS and run the heap out. `recv_size` stays, for the window
 * the block's acknowledgements give.
 */
static void tcp_time_wait_trim(struct tcb *tcb)
{
    if (tcb->state != TCP_TIME_WAIT) {
        return;
    }
    if (tcb->send_buf) {
        kfree(tcb->send_buf);
        tcb->send_buf = NULL;
    }
    if (tcb->recv_buf && (!tcb->sock || tcb->recv_tail == tcb->recv_head)) {
        kfree(tcb->recv_buf);
        tcb->recv_buf = NULL;
        if (tcb->recv_seg_heap) {
            kfree(tcb->recv_seg_heap);
            tcb->recv_seg_heap = NULL;
            tcb->recv_seg_cap = 0;
        }
        tcb->recv_seg_first = tcb->recv_seg_count = 0;
    }
}

/**
 * The most blocks the TIME_WAIT run holds, as Linux 6.12 sizes
 * tcp_max_tw_buckets: half the buckets of its established hash, which has
 * the largest power of two of buckets at or below one for each 32 frames
 * of memory, 128 KiB, and from 512 to 524288 of them. Linux on i386 counts
 * only its low memory; this counts every frame on both architectures.
 */
static uint32_t tcp_tw_max(void)
{
    if (!tw_max) {
        size_t want = frame_total_count() >> 5;
        uint32_t buckets = 512;

        while (buckets < 524288u && (size_t)buckets * 2u <= want) {
            buckets *= 2u;
        }
        tw_max = buckets / 2u;
    }
    return tw_max;
}

/**
 * Put `tcb`, a detached block in TIME_WAIT, in the TIME_WAIT run at the
 * list's end, after every block of the run whose TIME_WAIT ends no later:
 * at the end itself for a block that has just begun or restarted its
 * TIME_WAIT, which is the common case and costs no search.
 */
static void tcp_tw_list(struct tcb *tcb)
{
    struct tcb *after;

    tcb_unlink(tcb);
    after = tcb_last;
    while (after && after->tw_listed &&
           (int32_t)(after->timewait_until - tcb->timewait_until) > 0) {
        after = after->prev;
    }
    tcb->prev = after;
    tcb->next = after ? after->next : tcbs;
    if (tcb->next) {
        tcb->next->prev = tcb;
    } else {
        tcb_last = tcb;
    }
    if (after) {
        after->next = tcb;
    } else {
        tcbs = tcb;
    }
    if (!after || !after->tw_listed) {
        tw_first = tcb;
    }
    tcb->tw_listed = 1;
    tw_count++;
}

/* Whether the TIME_WAIT run's overflow has been logged; Linux logs each,
 * rate-limited, and one line says what the counter then counts. */
static int tw_overflow_logged;

/**
 * Move `tcb` to TIME_WAIT for TCP_2MSL_TICKS with a fresh ACK rate limit,
 * trim its buffers and, when no socket holds it, add it to the TIME_WAIT
 * run. With the run full (`tcp_tw_max`) the TIME_WAIT ends at once,
 * counted in TCPTimeWaitOverflow, as Linux's tcp_time_wait closes a
 * connection it has no bucket for. The block is freed at the next tick,
 * or at its socket's close.
 */
static void tcp_enter_time_wait(struct tcb *tcb)
{
    tcb->state = TCP_TIME_WAIT;
    tcb->delack_due = 0;
    tcb->oow_stamp = 0;
    tcb->timewait_until = TCP_NOW() + TCP_2MSL_TICKS;
    tcp_time_wait_trim(tcb);
    if (tw_count >= tcp_tw_max()) {
        net_mib.tcp_time_wait_overflow++;
        if (!tw_overflow_logged) {
            tw_overflow_logged = 1;
            klog(KLOG_INFO, "tcp",
                 "time wait: the table of %u is full, and connections skip TIME_WAIT; "
                 "TCPTimeWaitOverflow counts them",
                 (unsigned)tcp_tw_max());
        }
        tcb->timewait_until = TCP_NOW();
        return;
    }
    if (!tcb->sock) {
        tcp_tw_list(tcb);
    }
}

/**
 * Start the 60 s of `tcb`'s TIME_WAIT again, as Linux's
 * inet_twsk_reschedule does, keeping the block at the run's end, which is
 * ordered by the time each TIME_WAIT ends. A block that skipped TIME_WAIT
 * for a full run (`tcp_enter_time_wait`) is left to end at the next tick.
 */
static void tcp_time_wait_restart(struct tcb *tcb)
{
    if (tcb->tw_listed || tcb->sock) {
        tcb->timewait_until = TCP_NOW() + TCP_2MSL_TICKS;
    }
    if (tcb->tw_listed) {
        tcp_tw_list(tcb);
    }
}

/**
 * End the TIME_WAIT of `tcb` at once, as Linux's inet_twsk_deschedule_put
 * ends it for a reset or a new SYN: a detached block is freed, and one a
 * socket still holds leaves the table of connections as CLOSED, with no
 * error, for the socket's close to free, so that the next segment for its
 * pair finds the listener. The caller must not use a freed `tcb`.
 */
static void tcp_time_wait_kill(struct tcb *tcb)
{
    if (!tcb->sock) {
        tcb_free(tcb);
        return;
    }
    tcb_hash_del(tcb);
    tcp_closed_or_free(tcb);
}

/**
 * Take a segment other than a reset that starts at `seq` and ends before
 * `end`, its SYN and FIN counted, for `tcb` in TIME_WAIT, as Linux 6.12's
 * tcp_timewait_state_process takes one without timestamps (RFC 1122,
 * 4.2.2.13). One at `rcv_nxt` that occupies nothing starts the 60 s again.
 * A SYN without ACK past `rcv_nxt` may open the pair again, which
 * `tcp_time_wait_reopen` does. Any other segment starts the 60 s again when
 * it has ACK and is acknowledged, as Linux's
 * tcp_timewait_check_oow_rate_limit answers it: one with data or a FIN and
 * no SYN always, a FIN sent again among them, and any other only when
 * `tcp_oow_limited` allows, counted in TCPACKSkippedTimeWait when not.
 * Returns 1 for the SYN that may open the pair again, or 0 when the
 * segment ends here. The caller holds `net_lock`.
 */
static int tcp_rx_time_wait(struct tcb *tcb, uint8_t flags, uint32_t seq, uint32_t end)
{
    if (seq == tcb->rcv_nxt && end == seq) {
        tcp_time_wait_restart(tcb);
        return 0;
    }
    if ((flags & (TCP_SYN | TCP_ACK)) == TCP_SYN && (int32_t)(seq - tcb->rcv_nxt) > 0) {
        return 1;
    }
    if (flags & TCP_ACK) {
        tcp_time_wait_restart(tcb);
    }
    if (((flags & TCP_SYN) || end == seq) &&
        tcp_oow_limited(tcb, &net_mib.tcp_ack_skipped_time_wait)) {
        return 0;
    }
    tcp_send_flags(tcb, TCP_ACK);
    return 0;
}

/**
 * Open the pair of `tcb`, in TIME_WAIT, again for a SYN to `dst_ip`:
 * `dst_port` that `tcp_rx_time_wait` let through, as Linux's tcp_v4_rcv
 * and tcp_v6_rcv take TCP_TW_SYN: when a listener has the port
 * (`tcp_find_listener`), the TIME_WAIT ends (`tcp_time_wait_kill`) and the
 * listener is returned, with in `*isn` the initial sequence number its
 * connection must take, 65537 past the last one sent here, so that the new
 * connection's numbers lie above the old one's. With none the
 * SYN is acknowledged and NULL returned. The caller holds `net_lock`.
 */
static struct tcb *tcp_time_wait_reopen(struct tcb *tcb, net_addr_t dst_ip, uint16_t dst_port,
                                        uint32_t *isn)
{
    struct tcb *listener = tcp_find_listener(dst_ip, dst_port);

    if (!listener) {
        tcp_send_flags(tcb, TCP_ACK);
        return NULL;
    }
    *isn = tcb->snd_nxt + 65535u + 2u;
    tcp_time_wait_kill(tcb);
    return listener;
}

/**
 * Whether `tcb` must be ended with a reset at its socket's close, as
 * Linux's __tcp_close ends a connection whose receive queue held a byte
 * past copied_seq (net/ipv4/tcp.c:3085-3093): data received and never read,
 * from ESTABLISHED to LAST_ACK. A block in TIME_WAIT or CLOSED with its
 * socket still attached is one Linux's full socket has left for TCP_CLOSE
 * already (tcp_time_wait, tcp_done), which its close frees with no reset.
 * Bytes held past a hole do not count, as Linux's out-of-order queue does
 * not.
 */
static int tcp_unread_on_close(const struct tcb *tcb)
{
    return tcb->state >= TCP_ESTABLISHED && tcb->state != TCP_TIME_WAIT &&
           tcb->recv_tail != tcb->recv_head;
}

/**
 * End `tcb`, whose socket closed with received data unread, with a reset
 * and free it, as Linux's __tcp_close counts TCPAbortOnClose, moves to
 * TCP_CLOSE and sends tcp_send_active_reset (net/ipv4/tcp.c:3109-3113): an
 * RST and ACK at snd_nxt, tcp_acceptable_seq's answer while the peer's
 * window covers it (net/ipv4/tcp_output.c:3661-3662), and RFC 1122,
 * 4.2.2.13, has the peer told that data was lost. The move to TCP_CLOSE
 * counts EstabResets from ESTABLISHED and CLOSE_WAIT (`tcp_count_end`).
 * The caller must not use `tcb` afterwards.
 */
static void tcp_abort_on_close(struct tcb *tcb)
{
    net_mib.tcp_abort_on_close++;
    (void)tcp_emit_flags(tcb, TCP_RST | TCP_ACK, tcb->snd_nxt);
    tcp_count_end(tcb->state);
    tcb_free(tcb);
}

/**
 * FIN_WAIT_1, CLOSING, LAST_ACK and TIME_WAIT follow a shutdown: the FIN
 * went, and the block finishes the close without its socket, as it does
 * after a close. A detached block that still sends starts its limit,
 * TCP_ORPHAN_TICKS, here. Data the reader left unread ends the connection
 * with a reset instead (`tcp_abort_on_close`).
 */
void tcp_close(struct socket *sock)
{
    struct tcb *tcb;
    uint32_t flags;

    if (!sock || !sock->tcb)
        return;
    tcb = sock->tcb;

    flags = net_lock();

    sock->tcb = NULL;
    tcb->sock = NULL;
    if (tcb->state == TCP_LISTEN) {
        tcp_free_half_opens(tcb);
    }
    if (tcp_unread_on_close(tcb)) {
        tcp_abort_on_close(tcb);
        net_unlock(flags);
        return;
    }

    switch (tcb->state) {
    case TCP_ESTABLISHED:
    case TCP_CLOSE_WAIT:
        tcb->timewait_until = TCP_NOW() + TCP_ORPHAN_TICKS;
        tcb->fin_pending = 1;
        tcp_pump(tcb);
        break;
    case TCP_FIN_WAIT_2:
        tcb->timewait_until = TCP_NOW() + TCP_FIN_WAIT2_TICKS;
        break;
    case TCP_FIN_WAIT_1:
    case TCP_CLOSING:
    case TCP_LAST_ACK:
        tcb->timewait_until = TCP_NOW() + TCP_ORPHAN_TICKS;
        break;
    case TCP_TIME_WAIT:
        if ((int32_t)(TCP_NOW() - tcb->timewait_until) >= 0) {
            tcb_free(tcb);
            break;
        }
        tcp_time_wait_trim(tcb);
        tcp_tw_list(tcb);
        break;
    case TCP_CLOSED:
    case TCP_LISTEN:
    case TCP_SYN_SENT:
    case TCP_SYN_RCVD:
    default:
        tcb_free(tcb);
        break;
    }

    net_unlock(flags);
}

/**
 * The states are numbered so that ESTABLISHED and every closing state compare
 * at least TCP_ESTABLISHED; the handshake states, LISTEN and CLOSED have no
 * peer. `sock->tcb` is read once, so that the block whose state is read is
 * the one checked for NULL.
 */
int tcp_is_connected(struct socket *sock)
{
    struct tcb *tcb;

    if (!sock || sock->type != SOCK_STREAM || sock->listening) {
        return 0;
    }
    tcb = sock->tcb;
    if (!tcb) {
        return 0;
    }
    return tcb->state >= TCP_ESTABLISHED;
}

/**
 * A segment received sets `rcv_wnd` again, so the peer learns of a window
 * that shrank. A window grows when the reader moves `recv_head` in a system
 * call, which sends nothing, so it is announced here; without the
 * announcement a peer that filled the ring waits for its own persist timer
 * before probing again.
 *
 * FIN_WAIT_1 and FIN_WAIT_2 are among the states updated, since a reader
 * that shut down its sending half goes on reading. CLOSE_WAIT is not: it is
 * entered on the peer's FIN, after which the peer sends no data. Nor is a
 * socket shut for receiving, as Linux's tcp_cleanup_rbuf announces no window
 * after RCV_SHUTDOWN (net/ipv4/tcp.c:1489). The lock is held from the read
 * of `sock->tcb` through the send, so that the block sent from is the one
 * checked.
 *
 * Under `tcp_delack` the read follows the rest of tcp_cleanup_rbuf. A held
 * acknowledgement goes out at once when more than `rcv_mss` is
 * unacknowledged, or when the read emptied the ring after a segment that
 * ended a write of the peer's (`ack_pushed`), in ping-pong mode only after
 * two, since there the answer will carry it. Otherwise a window is
 * announced only while the peer has no more than half the ring left, and
 * only when the new one is at least twice what it has.
 */
/**
 * Whether a read makes the acknowledgement `tcb` holds due, as Linux's
 * tcp_cleanup_rbuf judges it: more than `rcv_mss` is unacknowledged, or
 * the ring is empty after a segment that ended a write of the peer's, two
 * in ping-pong mode.
 */
static int tcp_read_acks(const struct tcb *tcb)
{
    return tcb->delack_due != 0 &&
           (tcb->rcv_nxt - tcb->rcv_acked > tcb->rcv_mss ||
            ((tcb->ack_pushed == 2 || (tcb->ack_pushed == 1 && !tcb->pingpong)) &&
             tcb->recv_tail == tcb->recv_head));
}

/**
 * The window a read that reopened the ring of `tcb` announces, or 0 to
 * stay quiet. Under `tcp_delack` it is Linux's tcp_cleanup_rbuf: the
 * window as it stands, in whole units of the window scale, announced while
 * the peer has no more than half the largest window left and when it is at
 * least twice what the peer has, and never below `rcv_mss` or a sixteenth
 * of the largest window, as __tcp_select_window offers none smaller.
 * Otherwise it is RFC 1122's rule of `rcvwnd_update`.
 */
static uint32_t tcp_read_window(const struct tcb *tcb)
{
    uint32_t most = (uint32_t)TCP_WINDOW_SIZE << tcb->rcv_wscale;
    uint32_t wnd, left, clamp;

    if (!tcp_delack || !tcb->rcv_adv_set) {
        return rcvwnd_update(tcb->rcv_wnd, tcb->recv_tail - tcb->recv_head, tcb->recv_size, most,
                             TCP_MSS);
    }
    wnd = tcp_rcv_window(tcb) & ~((1u << tcb->rcv_wscale) - 1u);
    left = tcb->rcv_adv - tcb->rcv_nxt;
    clamp = most > tcb->recv_size ? tcb->recv_size : most;
    if ((int32_t)left < 0) {
        left = 0;
    }
    if (wnd < tcb->rcv_mss || wnd < clamp / 16u || 2u * left > clamp || wnd < 2u * left) {
        return 0;
    }
    return wnd;
}

/**
 * The work of a read on `sock`'s connection `tcb`, Linux's tcp_cleanup_rbuf:
 * send the acknowledgement the read makes due (`tcp_read_acks`), or else,
 * in ESTABLISHED, FIN_WAIT_1 and FIN_WAIT_2 and while the socket is not
 * shut for receiving, announce the window the read reopened
 * (`tcp_read_window`). With `net_lock` held.
 */
static void tcp_cleanup_rbuf(struct socket *sock, struct tcb *tcb)
{
    uint32_t wnd;

    if (tcp_read_acks(tcb)) {
        tcb->rcv_wnd = tcp_rcv_window(tcb);
        tcp_send_flags(tcb, TCP_ACK);
        return;
    }
    if ((sock->shutdown & SOCK_RCV_SHUTDOWN) ||
        (tcb->state != TCP_ESTABLISHED && tcb->state != TCP_FIN_WAIT_1 &&
         tcb->state != TCP_FIN_WAIT_2)) {
        return;
    }
    wnd = tcp_read_window(tcb);
    if (wnd != 0u) {
        tcb->rcv_wnd = wnd;
        tcp_send_flags(tcb, TCP_ACK);
    }
}

void tcp_window_update(struct socket *sock)
{
    struct tcb *tcb;
    uint32_t flags;

    if (!sock) {
        return;
    }

    flags = net_lock();
    tcb = sock->tcb;
    if (tcb && (tcb->state == TCP_ESTABLISHED || tcb->state == TCP_FIN_WAIT_1 ||
                tcb->state == TCP_FIN_WAIT_2)) {
        tcp_cleanup_rbuf(sock, tcb);
    }
    net_unlock(flags);
}

/**
 * TCP_QUICKACK, as Linux's __tcp_sock_set_quickack: 0 puts the connection
 * in ping-pong mode; any other value ends it and, in ESTABLISHED or
 * CLOSE_WAIT with an acknowledgement held, does what a read does with the
 * held acknowledgement taken as ending a write of the peer's
 * (`tcp_cleanup_rbuf`), so that it goes when the ring is empty or more
 * than `rcv_mss` is unacknowledged; then an even value puts the mode back.
 * A socket with no connection keeps the mode for the one `connect` opens.
 */
void tcp_quickack_set(struct socket *sock, int val)
{
    uint32_t flags = net_lock();
    struct tcb *tcb = sock->tcb;

    sock->quickack_off = val == 0;
    if (tcb && !val) {
        tcb->pingpong = 1;
    } else if (tcb) {
        tcb->pingpong = 0;
        if ((tcb->state == TCP_ESTABLISHED || tcb->state == TCP_CLOSE_WAIT) &&
            tcb->delack_due != 0) {
            if (tcb->ack_pushed == 0) {
                tcb->ack_pushed = 1;
            }
            tcp_cleanup_rbuf(sock, tcb);
            if (!(val & 1)) {
                tcb->pingpong = 1;
            }
        }
    }
    net_unlock(flags);
}

int tcp_quickack_get(const struct socket *sock)
{
    uint32_t flags = net_lock();
    int val = sock->tcb ? !sock->tcb->pingpong : !sock->quickack_off;

    net_unlock(flags);
    return val;
}

/**
 * Move `snd_una` and `send_ack` for an acknowledgement inside the window, in
 * a state where this side's FIN may be outstanding. The FIN's sequence
 * number is not data in the ring, so `send_ack` moves by the data
 * acknowledged only, at most the ring's sent bytes; this keeps the
 * retransmission timer from sending acknowledged data again after the FIN is
 * sent. The caller has checked snd_una < ack_num <= snd_nxt. A fast resend
 * still owed is dropped, since its hole was at the old `snd_una`. A SYN
 * still unacknowledged (`syn_unacked`) is acknowledged by any such number,
 * and its slot in the ring and its retransmission go with it.
 *
 * An acknowledgement that reaches `recover` ends a loss recovery, a fast one
 * with the congestion window set to the threshold. Outside a recovery
 * `recover` follows the acknowledged data, one byte behind it, so the test
 * that keeps duplicates of a recovery's last acknowledgement from beginning
 * another stays in range over any length of stream.
 */
static void tcp_ack_data(struct tcb *tcb, uint32_t ack_num)
{
    uint32_t acked = ack_num - tcb->snd_una;
    uint32_t ring = tcb->send_head - tcb->send_ack;

    tcb->send_ack += (acked < ring) ? acked : ring;
    tcb->snd_acked = acked > 0xFFFFFFFFu - tcb->snd_acked ? 0xFFFFFFFFu : tcb->snd_acked + acked;
    if (tcb->syn_unacked) {
        tcb->syn_unacked = 0;
        if (tcb->retx_flags & TCP_SYN) {
            tcb->retx_flags = 0;
        }
    }
    tcb->snd_una = ack_num;
    tcp_sent_acked(tcb);
    tcb->soft_error = 0;
    tcb->dup_acks = 0;
    tcb->fast_pending = 0;
    if (tcb->recovering == TCP_RECOVER_RTO && (int32_t)(tcb->retx_nxt - ack_num) < 0) {
        tcb->retx_nxt = ack_num;
    }
    if (tcb->recovering && (int32_t)(ack_num - tcb->recover) >= 0) {
        if (tcb->recovering == TCP_RECOVER_FAST) {
            tcb->cwnd = tcb->ssthresh;
        }
        tcb->recovering = 0;
    } else if (!tcb->recovering) {
        tcb->recover = ack_num - 1;
    }
}

/**
 * Fold a round trip of `us` microseconds into the estimate `ss -i` reports,
 * by RFC 6298's weights, as the tick estimate beside it is folded.
 */
static void tcp_rtt_us(struct tcb *tcb, uint32_t us)
{
    uint32_t delta;

    if (us == 0) {
        us = 1;
    }
    if (tcb->srtt_us == 0) {
        tcb->srtt_us = us << 3;
        tcb->rttvar_us = us << 1;
        return;
    }
    delta = (tcb->srtt_us >> 3) > us ? (tcb->srtt_us >> 3) - us : us - (tcb->srtt_us >> 3);
    tcb->rttvar_us = tcb->rttvar_us - (tcb->rttvar_us >> 2) + delta;
    tcb->srtt_us = tcb->srtt_us - (tcb->srtt_us >> 3) + us;
}

/**
 * Take an acknowledgement of new data, in any state where sent data awaits
 * one: the round-trip sample it ends, the ring and the sequence space, the
 * peer's window, the congestion window's growth or the recovery's next step,
 * the timer, a detached block's limit, and then whatever can be sent. The
 * timeouts in a row start again (`rto_backoff`), but their give-up keeps
 * counting from the loss's first retransmission (`retx_stamp`) until the
 * recovery ends, as Linux keeps retrans_stamp while data sent again is
 * unacknowledged. In a
 * fast recovery that the acknowledgement does not end, the next hole is sent
 * at once and the congestion window is deflated by the bytes acknowledged,
 * plus one segment (RFC 6582). The acknowledgement of a probe of a larger
 * size makes that size the connection's. The caller has checked that the
 * acknowledgement is inside (snd_una, snd_nxt]. The writers of the socket
 * are woken, since the ring has room.
 */
static void tcp_take_ack(struct tcb *tcb, uint32_t ack_num, uint32_t window)
{
    uint32_t acked = ack_num - tcb->snd_una;
    int was_fast = tcb->recovering == TCP_RECOVER_FAST;

    if (tcb->rtt_start != 0 && (int32_t)(ack_num - tcb->rtt_seq) >= 0) {
        uint32_t rtt = TCP_NOW() - (tcb->rtt_start - 1);
        uint32_t new_rto, delta;
        if (rtt == 0)
            rtt = 1;
        if (tcb->srtt == 0) {
            tcb->srtt = rtt << 3;
            tcb->rttvar = rtt << 1;
        } else {
            delta = ((tcb->srtt >> 3) > rtt) ? (tcb->srtt >> 3) - rtt : rtt - (tcb->srtt >> 3);
            tcb->rttvar = tcb->rttvar - (tcb->rttvar >> 2) + delta;
            tcb->srtt = tcb->srtt - (tcb->srtt >> 3) + rtt;
        }
        new_rto = (tcb->srtt >> 3) + tcb->rttvar;
        if (new_rto < TCP_RTO_MIN)
            new_rto = TCP_RTO_MIN;
        if (new_rto > TCP_RTO_MAX)
            new_rto = TCP_RTO_MAX;
        tcb->rto = new_rto;
        tcb->rtt_start = 0;
        tcp_rtt_us(tcb, timer_now_us() - tcb->rtt_start_us);
    }

    tcp_ack_data(tcb, ack_num);
    tcp_take_wnd(tcb, window);
    tcb->rto_backoff = 0;
    if (!tcb->recovering) {
        tcb->retx_stamp = 0;
    }
    if (tcb->probe_size != 0 && (int32_t)(ack_num - (tcb->probe_seq + tcb->probe_size)) >= 0) {
        tcb->snd_mss = tcb->probe_size;
        tcb->probe_size = 0;
        tcb->probe_after = TCP_NOW();
        net_mib.tcp_mtup_success++;
    }
    if (!tcb->sock) {
        tcb->timewait_until = TCP_NOW() + TCP_ORPHAN_TICKS;
    }

    if (was_fast) {
        if (tcb->recovering) {
            tcb->cwnd = (tcb->cwnd > acked ? tcb->cwnd - acked : 0) + tcb->snd_mss;
            tcb->fast_pending = tcp_resend(tcb, tcb->snd_una, tcb->snd_mss) < 0;
        }
    } else {
        tcp_ack_grow(tcb);
    }

    if (tcb->snd_una == tcb->snd_nxt) {
        tcb->rto_due = 0;
        tcb->zwp_due = 0;
    } else {
        tcb->rto_due = tcp_due(tcb->rto);
    }
    tcp_arm_zwp(tcb);
    tcp_pump(tcb);
    if (tcb->sock) {
        socket_tcp_writable(tcb->sock);
    }
}

/**
 * Grow the congestion window for an acknowledgement of new data (RFC
 * 5681): a segment per acknowledgement in slow start, up to the threshold,
 * and a segment per window after it.
 */
static void tcp_ack_grow(struct tcb *tcb)
{
    if (tcb->cwnd < tcb->ssthresh) {
        tcb->cwnd += tcb->snd_mss;
        if (tcb->cwnd > tcb->ssthresh)
            tcb->cwnd = tcb->ssthresh;
    } else {
        uint32_t inc = (uint32_t)tcb->snd_mss * tcb->snd_mss / tcb->cwnd;
        if (inc == 0)
            inc = 1;
        tcb->cwnd += inc;
    }
}

/**
 * Take `len` bytes of in-order data and return how many to move `rcv_nxt`
 * by. A socket gets the data, and may take less than `len` when its receive
 * ring is nearly full, so that the peer sends the rest again. A block whose
 * socket `tcp_close` detached has no reader, and new data ends it with a
 * reset before it gets here (`tcp_rx_closed_data`); were any to come, it
 * would be taken whole and dropped.
 */
static uint16_t tcp_accept_data(struct tcb *tcb, const uint8_t *payload, uint16_t len)
{
    if (tcb->sock) {
        return socket_tcp_deliver(tcb->sock, payload, len);
    }
    return len;
}

/**
 * Record that the sequence numbers [start, end) are kept past a hole, merged
 * with the ranges they overlap or touch. Returns 1, or 0 for a range that
 * would be one more than TCP_OOO_RANGES, which is not recorded and whose
 * bytes the peer sends again.
 */
static int tcp_ooo_add(struct tcb *tcb, uint32_t start, uint32_t end)
{
    int i, j;

    for (i = 0; i < tcb->ooo_count; i++) {
        if ((int32_t)(end - tcb->ooo_start[i]) < 0) {
            break;
        }
        if ((int32_t)(start - tcb->ooo_end[i]) <= 0) {
            if ((int32_t)(start - tcb->ooo_start[i]) < 0)
                tcb->ooo_start[i] = start;
            if ((int32_t)(end - tcb->ooo_end[i]) > 0)
                tcb->ooo_end[i] = end;
            while (i + 1 < tcb->ooo_count &&
                   (int32_t)(tcb->ooo_start[i + 1] - tcb->ooo_end[i]) <= 0) {
                if ((int32_t)(tcb->ooo_end[i + 1] - tcb->ooo_end[i]) > 0)
                    tcb->ooo_end[i] = tcb->ooo_end[i + 1];
                for (j = i + 1; j + 1 < tcb->ooo_count; j++) {
                    tcb->ooo_start[j] = tcb->ooo_start[j + 1];
                    tcb->ooo_end[j] = tcb->ooo_end[j + 1];
                }
                tcb->ooo_count--;
            }
            return 1;
        }
    }
    if (tcb->ooo_count == TCP_OOO_RANGES) {
        return 0;
    }
    for (j = tcb->ooo_count; j > i; j--) {
        tcb->ooo_start[j] = tcb->ooo_start[j - 1];
        tcb->ooo_end[j] = tcb->ooo_end[j - 1];
    }
    tcb->ooo_start[i] = start;
    tcb->ooo_end[i] = end;
    tcb->ooo_count++;
    return 1;
}

/** Forget the first kept range, which rcv_nxt has reached. */
static void tcp_ooo_drop_first(struct tcb *tcb)
{
    int j;

    for (j = 0; j + 1 < tcb->ooo_count; j++) {
        tcb->ooo_start[j] = tcb->ooo_start[j + 1];
        tcb->ooo_end[j] = tcb->ooo_end[j + 1];
    }
    tcb->ooo_count--;
}

/** Note `seq`, the start of a segment just kept past a hole, as the newest
 * of the segments whose ranges an acknowledgement names first. */
static void tcp_sack_note(struct tcb *tcb, uint32_t seq)
{
    int i;

    if (tcb->sack_recent_count < TCP_SACK_BLOCKS) {
        tcb->sack_recent_count++;
    }
    for (i = tcb->sack_recent_count - 1; i > 0; i--) {
        tcb->sack_recent[i] = tcb->sack_recent[i - 1];
    }
    tcb->sack_recent[0] = seq;
}

/**
 * Take the `len` bytes of data at `payload` of a segment that starts at
 * `seq`, with a FIN after them when `fin` is set, as Linux's tcp_data_queue
 * takes it (RFC 9293, section 3.10.7.4). Bytes before rcv_nxt were taken
 * already and are skipped; a segment that ends at or before rcv_nxt, its
 * FIN counted, was taken whole and goes no further (`tcp_rx_resent` counts
 * it). Data that starts at or past the right edge of the windows offered
 * (`rcv_adv`) is dropped, as tcp_data_queue drops it, a closed window's
 * above all: free space that rounds down to a window of 0 takes no
 * zero-window probe of one byte, and a segment whose data was all taken
 * before loses its FIN with the rest. Data at rcv_nxt is delivered, with
 * the kept ranges it reaches; data past it, which a lost segment left a
 * hole before, is kept in the receive ring, so that the resend of the lost
 * segment makes the whole run readable and the peer, whose acknowledgement
 * then covers it, does not send it again. A segment taken at rcv_nxt
 * stamps `ka_since`, as tcp_event_data_recv stamps lrcvtime for a segment
 * with data queued, even when only its FIN is new; a FIN without data
 * stamps nothing here. The caller acknowledges the segment either way.
 *
 * @return 0 when the window dropped the segment, whose FIN must then not
 * be taken; 1 otherwise.
 */
static int tcp_take_data(struct tcb *tcb, uint32_t seq, const uint8_t *payload, uint16_t len,
                         int fin)
{
    uint32_t skip = 0, n;
    uint16_t took;

    if ((int32_t)(seq - tcb->rcv_nxt) < 0) {
        skip = tcb->rcv_nxt - seq;
        if (skip > len || (skip == len && !fin)) {
            return 1;
        }
        payload += skip;
        len = (uint16_t)(len - skip);
        seq = tcb->rcv_nxt;
    }
    if (tcb->rcv_adv_set && (int32_t)(seq - tcb->rcv_adv) >= 0) {
        tcp_enter_quickack(tcb); /* Linux's out_of_window, a probe's too */
        return 0;
    }
    if (seq != tcb->rcv_nxt) {
        if (tcb->sock) {
            took = socket_tcp_hold(tcb->sock, seq - tcb->rcv_nxt, payload, len);
            if (took > 0 && tcp_ooo_add(tcb, seq, seq + took)) {
                tcp_sack_note(tcb, seq);
            }
        }
        return 1;
    }
    took = len > 0 ? tcp_accept_data(tcb, payload, len) : 0;
    tcb->rcv_nxt += took;
    if (took > 0 || (len == 0 && skip > 0)) {
        tcb->ka_since = TCP_NOW();
    }
    while (tcb->ooo_count > 0 && (int32_t)(tcb->ooo_start[0] - tcb->rcv_nxt) <= 0) {
        if ((int32_t)(tcb->ooo_end[0] - tcb->rcv_nxt) > 0) {
            n = tcb->ooo_end[0] - tcb->rcv_nxt;
            socket_tcp_release(tcb->sock, n);
            tcb->rcv_nxt += n;
        }
        tcp_ooo_drop_first(tcb);
    }
    return 1;
}

/**
 * Free a passive block that never reached an accept queue, with its socket,
 * which no descriptor holds.
 */
static void tcp_free_half_open(struct tcb *tcb)
{
    struct socket *s = tcb->sock;

    tcb->sock = NULL;
    if (s) {
        s->tcb = NULL;
        socket_unref(s);
    }
    tcb_free(tcb);
}

/**
 * Free the passive handshakes made by `listener`, with their sockets.
 *
 * Nobody is left to accept them, and a listener opened on the port later
 * must not take a connection that began under this one, with this one's
 * owner; Linux drops them the same way. The peer's final acknowledgement then
 * finds no block: with no listener on the port it is answered with a reset,
 * and a listener opened since ignores a segment that is not a SYN.
 */
static void tcp_free_half_opens(const struct tcb *listener)
{
    struct tcb *t;
    struct tcb *next;

    for (t = tcbs; t && t != tw_first; t = next) {
        next = t->next;
        if (t->passive && t->state == TCP_SYN_RCVD && t->listener == listener) {
            tcp_free_half_open(t);
        }
    }
}

/**
 * Whether the connection `tcb`, past its handshake and before TIME_WAIT,
 * judges each segment by its receive window (`tcp_rx_check`), as Linux's
 * tcp_rcv_established and tcp_rcv_state_process judge it with
 * tcp_validate_incoming.
 */
static int tcp_synced(const struct tcb *tcb)
{
    return tcb->state >= TCP_ESTABLISHED && tcb->state != TCP_TIME_WAIT;
}

/**
 * Whether a segment that starts at `seq` and ends before `end`, its SYN and
 * FIN counted, is inside the receive window of `tcb`, as Linux's
 * tcp_sequence judges it: it does not end before `rcv_wup`, and does not
 * start past the right edge of the windows offered (`rcv_adv`), or past
 * `rcv_nxt` when that is later. Its data need not fit, so a probe of a
 * closed window, a byte at rcv_nxt, is inside. Before this side has
 * offered a window every segment is.
 */
static int tcp_in_window(const struct tcb *tcb, uint32_t seq, uint32_t end)
{
    uint32_t edge = tcb->rcv_nxt;

    if (!tcb->rcv_adv_set) {
        return 1;
    }
    if ((int32_t)(tcb->rcv_adv - edge) > 0) {
        edge = tcb->rcv_adv;
    }
    return (int32_t)(end - tcb->rcv_wup) >= 0 && (int32_t)(seq - edge) <= 0;
}

/**
 * Whether the acknowledgement that would answer a segment `tcb` cannot
 * accept must be held back, as Linux's __tcp_oow_rate_limited says: when
 * one went less than TCP_INVALID_RATELIMIT ago, which counts in
 * `*skipped`. Otherwise `oow_stamp` takes this tick, and it may go.
 */
static int tcp_oow_limited(struct tcb *tcb, uint32_t *skipped)
{
    uint32_t stamp = tcp_due(1), since = stamp - tcb->oow_stamp; /* now + 1, never 0 */

    if (tcb->oow_stamp != 0 && (int32_t)since >= 0 && since < TCP_INVALID_RATELIMIT) {
        (*skipped)++;
        return 1;
    }
    tcb->oow_stamp = stamp;
    return 0;
}

/**
 * Send `tcb`'s challenge acknowledgement (RFC 5961), which a true peer
 * answers with a segment this side accepts and a host off the path never
 * sees, as Linux's tcp_send_challenge_ack: at most one in
 * TCP_INVALID_RATELIMIT, counted in TCPChallengeACK, one held back in
 * TCPACKSkippedChallenge. Linux's limit across connections,
 * tcp_challenge_ack_limit, is off by default, and there is none here.
 */
static void tcp_challenge_ack(struct tcb *tcb)
{
    if (tcp_oow_limited(tcb, &net_mib.tcp_ack_skipped_challenge)) {
        return;
    }
    net_mib.tcp_challenge_ack++;
    tcp_send_flags(tcb, TCP_ACK);
}

/**
 * The right edge of the rightmost SACK block `tcb`'s acknowledgements name,
 * which a reset may also have (Linux's tcp_validate_incoming), or `rcv_nxt`
 * when they name none.
 */
static uint32_t tcp_sack_right(const struct tcb *tcb)
{
    uint8_t pick[TCP_SACK_BLOCKS];
    uint8_t n, i;
    uint32_t right = tcb->rcv_nxt;

    if (!tcb->sack_ok || tcb->ooo_count == 0) {
        return right;
    }
    n = tcp_sack_pick(tcb, pick);
    for (i = 0; i < n; i++) {
        if ((int32_t)(tcb->ooo_end[pick[i]] - right) > 0) {
            right = tcb->ooo_end[pick[i]];
        }
    }
    return right;
}

/* What `tcp_rst_verdict` allows a reset to do. */
#define TCP_RST_IGNORE    0
#define TCP_RST_CHALLENGE 1
#define TCP_RST_ACCEPT    2

/**
 * Judge a reset for `tcb` that starts at `seq` and ends before `end`, as
 * RFC 5961 (section 3.2) asks and Linux's tcp_validate_incoming judges it.
 *
 * A reset at `rcv_nxt` exactly is accepted, and so is one at `rcv_nxt` less
 * one after the peer's FIN, in CLOSE_WAIT, CLOSING or LAST_ACK, since a
 * peer that sent its FIN may reset from the FIN's number (tcp_reset_check),
 * and one at the right edge of the rightmost SACK block this side sends,
 * where a peer that took the acknowledgement for its whole stream may
 * reset. One elsewhere in the receive window (`tcp_in_window`) draws a
 * challenge acknowledgement (`tcp_challenge_ack`), which a true peer
 * answers with a reset at `rcv_nxt`, and one outside it is dropped: a host
 * off the path must then guess the sequence number itself rather than any
 * of the window's values. In SYN_SENT there is no receive window yet, and a
 * reset counts only when it acknowledges the SYN (RFC 793). In SYN_RCVD one
 * at `rcv_nxt` is accepted and one in the window last offered challenged.
 * TIME_WAIT accepts one at `rcv_nxt` alone, which ends it, as Linux's
 * tcp_timewait_state_process does with tcp_rfc1337 off, its default.
 * LISTEN and CLOSED have no connection to reset.
 *
 * @return TCP_RST_ACCEPT, TCP_RST_CHALLENGE or TCP_RST_IGNORE.
 */
static int tcp_rst_verdict(const struct tcb *tcb, uint32_t seq, uint32_t end, uint32_t ack_num,
                           uint8_t flags)
{
    switch (tcb->state) {
    case TCP_LISTEN:
    case TCP_CLOSED:
        return TCP_RST_IGNORE;
    case TCP_TIME_WAIT:
        return seq == tcb->rcv_nxt ? TCP_RST_ACCEPT : TCP_RST_IGNORE;
    case TCP_SYN_SENT:
        return ((flags & TCP_ACK) && ack_num == tcb->snd_nxt) ? TCP_RST_ACCEPT : TCP_RST_IGNORE;
    case TCP_SYN_RCVD:
        if (seq == tcb->rcv_nxt) {
            return TCP_RST_ACCEPT;
        }
        return seq - tcb->rcv_nxt < (uint32_t)tcb->rcv_wnd ? TCP_RST_CHALLENGE : TCP_RST_IGNORE;
    case TCP_CLOSE_WAIT:
    case TCP_CLOSING:
    case TCP_LAST_ACK:
        if (seq == tcb->rcv_nxt - 1u) {
            return TCP_RST_ACCEPT;
        }
        break;
    case TCP_ESTABLISHED:
    case TCP_FIN_WAIT_1:
    case TCP_FIN_WAIT_2:
    default:
        break;
    }
    if (seq == tcb->rcv_nxt) {
        return TCP_RST_ACCEPT;
    }
    if (!tcp_in_window(tcb, seq, end)) {
        return TCP_RST_IGNORE;
    }
    if (tcb->sack_ok && tcb->ooo_count > 0 && seq == tcp_sack_right(tcb)) {
        return TCP_RST_ACCEPT;
    }
    return TCP_RST_CHALLENGE;
}

/**
 * Take a reset for `tcb` that starts at `seq` and ends before `end` after
 * `tcp_rst_verdict` has judged it.
 *
 * A challenged reset is answered through `tcp_challenge_ack`, at most one
 * in TCP_INVALID_RATELIMIT, in SYN_RCVD too, whatever it carries, as
 * Linux's tcp_reqsk_send_challenge_ack and tcp_send_challenge_ack answer
 * it. An accepted one ends the connection (`tcp_reset`), or the TIME_WAIT
 * (`tcp_time_wait_kill`). The caller must not use `tcb` afterwards.
 */
static void tcp_rx_reset(struct tcb *tcb, uint32_t seq, uint32_t end, uint32_t ack_num,
                         uint8_t flags)
{
    int verdict = tcp_rst_verdict(tcb, seq, end, ack_num, flags);

    if (verdict == TCP_RST_CHALLENGE) {
        tcp_challenge_ack(tcb);
        return;
    }
    if (verdict != TCP_RST_ACCEPT) {
        return;
    }
    if (tcb->state == TCP_TIME_WAIT) {
        tcp_time_wait_kill(tcb);
        return;
    }
    tcp_reset(tcb);
}

/**
 * End the connection `tcb` as a reset this side accepted ends it, as
 * Linux's tcp_reset: counted in AttemptFails or EstabResets; a passive
 * block in SYN_RCVD is freed with its socket; any other connection is
 * closed with the error tcp_reset records, ECONNREFUSED in SYN_SENT, EPIPE
 * in CLOSE_WAIT, after the peer's FIN, and ECONNRESET otherwise, SYN_RCVD
 * after an active open among them, which SO_ERROR and POLLERR report; its
 * socket is woken, or the block is freed when it has no socket. Nothing is
 * sent. The caller must not use `tcb` afterwards.
 */
static void tcp_reset(struct tcb *tcb)
{
    tcp_count_end(tcb->state);
    if (tcb->state == TCP_SYN_RCVD && tcb->passive) {
        tcp_free_half_open(tcb);
        return;
    }
    tcb->so_error = tcb->state == TCP_SYN_SENT     ? ECONNREFUSED
                    : tcb->state == TCP_CLOSE_WAIT ? EPIPE
                                                   : ECONNRESET;
    tcb->state = TCP_CLOSED;
    tcp_delack_clear(tcb);
    tcb->retx_flags = 0;
    tcb->rto_due = 0;
    tcb->zwp_due = 0;
    tcb->recovering = 0;
    tcb->fast_pending = 0;
    if (tcb->sock) {
        socket_tcp_connected(tcb->sock);
        socket_tcp_fin(tcb->sock);
    } else {
        tcb_free(tcb);
    }
}

/**
 * Check an inbound segment's length, checksum and header length, and count
 * it in `net_mib`, a malformed one among the errors: its header length, or 0
 * for a segment to drop.
 */
static uint16_t tcp_seg_check(net_addr_t src_ip, net_addr_t dst_ip, const uint8_t *data,
                              uint16_t len)
{
    uint16_t hdr_len;

    net_mib.tcp_in_segs++;
    if (len < TCP_HDR_SIZE || len > TCP_SEG_MAX) {
        net_mib.tcp_in_errs++;
        return 0;
    }
    if (tcp_checksum(src_ip, dst_ip, data, len) != 0) {
        net_mib.tcp_in_errs++;
        klog(KLOG_WARN, "tcp", "rx: bad checksum, dropped");
        return 0;
    }
    hdr_len = (uint16_t)(((data[12] >> 4) & 0xF) * 4);
    if (hdr_len < TCP_HDR_SIZE || hdr_len > len) {
        net_mib.tcp_in_errs++;
        return 0;
    }
    return hdr_len;
}

/**
 * Answer a segment outside the receive window of `tcb`, with no reset or
 * SYN, that starts at `seq` and ends before `end`, its FIN counted, with a
 * duplicate acknowledgement, as Linux's tcp_validate_incoming answers it
 * with tcp_send_dupack. One with neither data nor a FIN is answered only
 * when `tcp_oow_limited` allows, and counts in TCPACKSkippedSeq when not,
 * since two ends that each find the other's acknowledgements stale would
 * otherwise answer each other at once and for ever; data or a FIN, which
 * such a loop does not carry, is always answered (tcp_oow_rate_limited).
 * Data that starts before rcv_nxt, a resend of what was taken, counts in
 * DelayedACKLost and enters quick mode (`tcp_enter_quickack`).
 */
static void tcp_dupack(struct tcb *tcb, uint32_t seq, uint32_t end)
{
    if (end == seq && tcp_oow_limited(tcb, &net_mib.tcp_ack_skipped_seq)) {
        return;
    }
    if (end != seq && (int32_t)(seq - tcb->rcv_nxt) < 0) {
        net_mib.tcp_delayed_ack_lost++;
        tcp_enter_quickack(tcb);
    }
    tcp_send_flags(tcb, TCP_ACK);
}

/**
 * Whether the acknowledgement `ack_num` may be taken by `tcb`, as Linux's
 * tcp_ack judges it, in its order. One below snd_una is old: it is refused
 * when it is further below than the largest window the peer has offered
 * (`snd_wnd_max`) or the bytes it has acknowledged (`snd_acked`), whichever
 * is less (RFC 5961, section 5.2), and otherwise the segment goes on with
 * it. One at or past snd_una is refused when it is past snd_nxt, which
 * acknowledges data never sent. A refused old one is answered with a
 * challenge acknowledgement (`tcp_challenge_ack`); one past snd_nxt is too,
 * but in ESTABLISHED, which Linux's tcp_rcv_established drops with no
 * answer. The bound is judged only below snd_una, so an acknowledgement
 * half the sequence space ahead is past snd_nxt, never too old.
 */
static int tcp_ack_ok(struct tcb *tcb, uint32_t ack_num)
{
    uint32_t most = tcb->snd_acked;

    if ((int32_t)(ack_num - tcb->snd_una) < 0) {
        if (most > tcb->snd_wnd_max) {
            most = tcb->snd_wnd_max;
        }
        if ((int32_t)(ack_num - (tcb->snd_una - most)) < 0) {
            tcp_challenge_ack(tcb);
            return 0;
        }
        return 1;
    }
    if ((int32_t)(ack_num - tcb->snd_nxt) > 0) {
        if (tcb->state != TCP_ESTABLISHED) {
            tcp_challenge_ack(tcb);
        }
        return 0;
    }
    return 1;
}

/**
 * Count in DelayedACKLost a segment of `flags` that starts at `seq` with
 * `len` bytes of data on the synchronized connection `tcb`, which
 * `tcp_rx_check` let on, when it has data or a FIN and was taken whole
 * before, its end at or before rcv_nxt: the peer sent it again because
 * this side's acknowledgement was lost, and enter quick mode
 * (`tcp_enter_quickack`). Any segment with data or a FIN schedules an
 * acknowledgement (`ack_sched`), as Linux's inet_csk_schedule_ack.
 * Linux's tcp_data_queue counts it, in ESTABLISHED, the FIN_WAIT states,
 * CLOSE_WAIT, and CLOSING and LAST_ACK unless the segment acknowledged
 * this side's FIN, which tcp_rcv_state_process ends the connection at
 * first; the arms of `tcp_rx_addr` call this in the same places.
 */
static void tcp_rx_resent(struct tcb *tcb, uint8_t flags, uint32_t seq, uint32_t len)
{
    uint32_t end = seq + len + ((flags & TCP_FIN) != 0);

    if (end != seq) {
        tcb->ack_sched = 1;
    }
    if (end != seq && (int32_t)(end - tcb->rcv_nxt) <= 0) {
        net_mib.tcp_delayed_ack_lost++;
        tcp_enter_quickack(tcb);
    }
}

/**
 * Judge a segment of `flags` that starts at `seq` with `len` bytes of data
 * and acknowledges `ack_num`, for a connection that `tcp_synced` says
 * judges it, before its acknowledgement or data is looked at, as Linux's
 * tcp_rcv_established and tcp_rcv_state_process judge it: one without ACK
 * is dropped; a SYN is dropped and answered with a challenge
 * acknowledgement (RFC 5961, section 4), counted in TCPSYNChallenge, and
 * in ESTABLISHED in InErrs; one outside the receive window
 * (`tcp_in_window`) is dropped and answered with a duplicate
 * acknowledgement (`tcp_dupack`); and one whose acknowledgement
 * `tcp_ack_ok` refuses is dropped. This is Linux's order: the flags,
 * tcp_validate_incoming's sequence check and its check of a SYN, then
 * tcp_ack's checks. A SYN is judged before the window, since Linux takes
 * one outside the window and one inside it to the same challenge. A reset
 * never comes here; `tcp_rx_reset` judges it as tcp_validate_incoming's
 * first two steps do.
 *
 * @return 1 when the segment goes on, 0 when it was dropped.
 */
static int tcp_rx_check(struct tcb *tcb, uint8_t flags, uint32_t seq, uint32_t len,
                        uint32_t ack_num)
{
    uint32_t end = seq + len + ((flags & TCP_FIN) != 0);

    if (!(flags & (TCP_ACK | TCP_SYN))) {
        return 0;
    }
    if (flags & TCP_SYN) {
        if (tcb->state == TCP_ESTABLISHED) {
            net_mib.tcp_in_errs++;
        }
        net_mib.tcp_syn_challenge++;
        tcp_challenge_ack(tcb);
        return 0;
    }
    if (!tcp_in_window(tcb, seq, end)) {
        tcp_dupack(tcb, seq, end);
        return 0;
    }
    return tcp_ack_ok(tcb, ack_num);
}

/**
 * Note what a segment of `flags` that acknowledges `ack_num` answers on the
 * connection `tcb`, which is not a listener's, once `tcp_rx_check` has let
 * it on. An acknowledgement inside [snd_una, snd_nxt], a duplicate among
 * them, is stamped in `rcv_stamp` and `ka_since` and answers the
 * keepalive's probes and the zero-window probes, as Linux's tcp_ack sets
 * rcv_tstamp and clears icsk_probes_out only past its checks of the
 * acknowledgement (`tcp_keepalive`, `tcp_zwp_expire`, `tcp_zwp_rtx_expired`).
 * Any other segment answers nothing here: one without ACK, one outside
 * the receive window or with an acknowledgement `tcp_ack_ok` refuses never
 * gets this far, and an old acknowledgement answers no probe, as Linux's
 * old_ack returns before them. In-order data it carries stamps `ka_since`
 * alone (`tcp_take_data`), as Linux's lrcvtime.
 */
static void tcp_rx_answers(struct tcb *tcb, uint8_t flags, uint32_t ack_num)
{
    if (tcb->state == TCP_LISTEN || !(flags & TCP_ACK) || (int32_t)(ack_num - tcb->snd_una) < 0 ||
        (int32_t)(ack_num - tcb->snd_nxt) > 0) {
        return;
    }
    tcb->rcv_stamp = TCP_NOW();
    tcb->ka_since = tcb->rcv_stamp;
    tcb->ka_probes = 0;
    tcb->zwp_probes = 0;
}

/**
 * End the connection `tcb` for a segment of `flags` that starts at `seq`
 * with `len` bytes of data and acknowledges `ack_num`, whose data runs past
 * what this side will read, counted in TCPAbortOnData: the connection ends
 * as an accepted reset ends it (`tcp_reset`), and the segment is answered
 * with a reset at `ack_num` (`tcp_send_rst_unmatched`), as Linux's
 * tcp_rcv_state_process returns SKB_DROP_REASON_TCP_ABORT_ON_DATA after
 * tcp_reset (tcp_input.c:7025-7030) and tcp_v4_do_rcv and tcp_v6_do_rcv
 * send the reset for it (tcp_ipv4.c:1944-1950, tcp_ipv6.c:1652-1660). The
 * caller must not use `tcb` afterwards.
 */
static void tcp_abort_on_data(struct tcb *tcb, uint8_t flags, uint32_t seq, uint32_t len,
                              uint32_t ack_num)
{
    net_mib.tcp_abort_on_data++;
    tcp_send_rst_unmatched(tcb->ifindex, tcb->local_ip, tcb->remote_ip, tcb->local_port,
                           tcb->remote_port, flags, seq, ack_num, len + ((flags & TCP_FIN) != 0));
    tcp_reset(tcb);
}

/**
 * End the connection `tcb`, shut for receiving after this side's FIN, for
 * a segment of `flags` that starts at `seq` with `len` bytes of data and
 * acknowledges `ack_num`, when its data or FIN runs past rcv_nxt
 * (`tcp_abort_on_data`), as Linux ends a socket with RCV_SHUTDOWN in
 * FIN_WAIT_1 and FIN_WAIT_2 (net/ipv4/tcp_input.c:7019-7031). RCV_SHUTDOWN
 * comes from the close (net/ipv4/tcp.c:3071), the block here detached, or
 * from shutdown(SHUT_RD) (`SOCK_RCV_SHUTDOWN`); after a close Linux also
 * ends it for the FIN acknowledged in the same segment
 * (tcp_input.c:6958-6966). RFC 1122, 4.2.2.13, has a reset sent for data
 * that no program will read. A block here in ESTABLISHED counts only with
 * its FIN queued behind its data, where Linux's close and shutdown have
 * moved to FIN_WAIT_1 already; without one, data after SHUT_RD alone is
 * taken, as Linux's ESTABLISHED queues it. Data or a FIN the peer sent
 * again, ending at or before rcv_nxt, is taken and acknowledged as before.
 *
 * @return 1 when the connection was ended, and `tcb` may be gone; 0
 * otherwise.
 */
static int tcp_rx_closed_data(struct tcb *tcb, uint8_t flags, uint32_t seq, uint32_t len,
                              uint32_t ack_num)
{
    if ((tcb->sock && !(tcb->sock->shutdown & SOCK_RCV_SHUTDOWN)) ||
        (tcb->state == TCP_ESTABLISHED && !tcb->fin_pending) || (len == 0 && !(flags & TCP_FIN)) ||
        (int32_t)(seq + len - tcb->rcv_nxt) <= 0) {
        return 0;
    }
    tcp_abort_on_data(tcb, flags, seq, len, ack_num);
    return 1;
}

/**
 * Answer a segment of `flags` that starts at `seq` with `len` bytes of
 * data and acknowledges `ack_num`, received after the peer's FIN
 * (CLOSE_WAIT, CLOSING or LAST_ACK), as Linux's tcp_rcv_state_process
 * answers it once its acknowledgement is judged (tcp_input.c:7005-7031).
 * One at or past rcv_nxt follows the FIN and is dropped with no answer.
 * One that starts before rcv_nxt and whose data runs past it, past the
 * peer's FIN, ends the connection and is answered with a reset
 * (`tcp_abort_on_data`): the peer sent data after it closed. Any other
 * with data or a FIN is one the peer sent again, its FIN above all, which
 * tcp_data_queue acknowledges at once and counts (`tcp_rx_resent`).
 *
 * @return 1 when the connection was ended, and `tcb` may be gone; 0
 * otherwise.
 */
static int tcp_rx_after_fin(struct tcb *tcb, uint8_t flags, uint32_t seq, uint32_t len,
                            uint32_t ack_num)
{
    if ((int32_t)(seq - tcb->rcv_nxt) >= 0 || (len == 0 && !(flags & TCP_FIN))) {
        return 0;
    }
    if ((int32_t)(seq + len - tcb->rcv_nxt) > 0) {
        tcp_abort_on_data(tcb, flags, seq, len, ack_num);
        return 1;
    }
    tcp_rx_resent(tcb, flags, seq, len);
    tcp_send_flags(tcb, TCP_ACK);
    return 0;
}

/**
 * Take `ack_num`, the acknowledgement that ends the handshake, and the
 * peer's window `window`, and enter ESTABLISHED. An acknowledgement of
 * this side's SYN ends the handshake's retransmission. The bytes
 * acknowledged (`snd_acked`) count the SYN on an active open, as Linux's
 * tcp_ack moves bytes_acked past it, and not on a passive one, whose child
 * Linux makes with snd_una past the SYN already.
 *
 * After a simultaneous open an acknowledgement of snd_una, the SYN's own
 * number, ends the handshake too, as Linux's tcp_ack takes it and
 * tcp_rcv_state_process enters ESTABLISHED (tcp_input.c:6880-6929), and
 * leaves the SYN unacknowledged: its retransmission goes on, the SYN-ACK
 * at snd_una, and the SYN holds the send ring's slot before the first byte
 * of data (`syn_unacked`), so that an acknowledgement of the SYN and data
 * moves the ring by both, as Linux's retransmission queue keeps the SYN
 * before the data. The ring is empty here, since data is written only
 * once the connection is established (`tcp_send`).
 */
static void tcp_handshake_done(struct tcb *tcb, uint32_t ack_num, uint32_t window)
{
    tcb->soft_error = 0;
    tcp_take_wnd(tcb, window);
    tcb->state = TCP_ESTABLISHED;
    tcb->retx_stamp = 0;
    if (ack_num == tcb->snd_una && ack_num != tcb->snd_nxt) {
        tcb->syn_unacked = 1;
        tcb->send_ack--; /* the SYN's slot */
        tcb->snd_acked = 0;
        return;
    }
    tcb->snd_una = ack_num;
    tcb->snd_acked = !tcb->passive;
    tcb->retx_flags = 0;
    tcb->rto_due = 0;
    tcb->rto_backoff = 0;
}

/**
 * Note a data segment of `payload_len` bytes, `hdr_len` bytes of header and
 * `flags` that moved `rcv_nxt`, as Linux's tcp_measure_rcv_mss and
 * tcp_event_data_recv do.
 *
 * A segment at least `rcv_mss` long sets `rcv_mss` to its length, no more
 * than TCP_MSS, and ends a write of the peer's when it has PSH. A shorter
 * one that is at least TCP_MSS_DEFAULT long with 20 bytes of header, its
 * header counted, or at least TCP_DELACK_TINY so with none of FIN, SYN, URG
 * and PSH, is a full segment of a smaller size, its length less 20, Linux's
 * fixed header with no option every segment carries: the second in a row
 * of one length becomes `rcv_mss`, which options that vary, such as SACK
 * blocks, do not upset. Any other shorter one ends a write, and a second
 * such counts twice (`ack_pushed`).
 *
 * The first data, and data after a quiet time longer than the
 * retransmission timeout, grant quick acknowledgements (`tcp_delack_quick`),
 * since the peer is starting its window again.
 */
static void tcp_delack_note(struct tcb *tcb, uint8_t flags, uint8_t hdr_len, uint16_t payload_len)
{
    uint32_t lss = tcb->rcv_last_seg, len = payload_len;
    uint32_t now = TCP_NOW();

    tcb->rcv_last_seg = 0;
    if (len >= tcb->rcv_mss) {
        tcb->rcv_mss = len > TCP_MSS ? TCP_MSS : len;
        if ((flags & TCP_PSH) && tcb->ack_pushed == 0) {
            tcb->ack_pushed = 1;
        }
    } else {
        len += hdr_len;
        if (len >= TCP_MSS_DEFAULT + 20u ||
            (len >= TCP_DELACK_TINY + 20u && !(flags & (TCP_FIN | TCP_SYN | TCP_URG | TCP_PSH)))) {
            len -= 20u;
            tcb->rcv_last_seg = len;
            if (len == lss) {
                tcb->rcv_mss = len;
                goto seen;
            }
        }
        tcb->ack_pushed = tcb->ack_pushed ? 2 : 1;
    }
seen:
    if (!tcb->rcv_data_seen || now - tcb->rcv_data_at > tcb->rto) {
        tcp_delack_quick(tcb);
    }
    tcb->rcv_data_at = now;
    tcb->rcv_data_seen = 1;
}

/**
 * Grant `tcb` quick acknowledgements for the window it last offered
 * (`rcv_offered`), as Linux's tcp_incr_quickack: one for each two full
 * segments the window holds, at most 16, or 2 when it holds fewer than
 * two, when that is more than it has left.
 */
static void tcp_delack_quick(struct tcb *tcb)
{
    uint32_t n = tcb->rcv_offered / (2u * tcb->rcv_mss);

    if (n == 0u) {
        n = 2u;
    }
    if (n > TCP_DELACK_QUICK) {
        n = TCP_DELACK_QUICK;
    }
    if (n > tcb->ack_quick) {
        tcb->ack_quick = (uint8_t)n;
    }
}

/**
 * Enter quick mode for data the peer sent again, as Linux's
 * tcp_enter_quickack_mode: grant quick acknowledgements and end ping-pong
 * mode, since the peer's resend says an acknowledgement went missing.
 */
static void tcp_enter_quickack(struct tcb *tcb)
{
    tcp_delack_quick(tcb);
    tcb->pingpong = 0;
}

/**
 * Whether the acknowledgement of the segment `data`, with `payload_len`
 * bytes of data, which found `rcv_nxt` at `expected` and data past a hole
 * when `had_ooo`, may wait, as Linux's __tcp_ack_snd_check lets it. Sets
 * the window the acknowledgement offers (`rcv_wnd`) first.
 *
 * Only data taken in order waits, with no FIN and with nothing past a hole
 * before or after. A segment wholly old entered quick mode already
 * (`tcp_rx_resent`).
 *
 * Inside a batch (`tcp_batch_begin`) it waits for the batch's end, and the
 * block joins the batch's list once; `tcp_batch_flush` keeps what one
 * acknowledgement covers within TCP_BATCH_ACK_MAX. Otherwise it waits
 * outside quick mode, while no `ack_quick` is left or the connection is in
 * ping-pong mode, and while no more than `rcv_mss` is unacknowledged, so
 * that every second full segment is acknowledged at once. A held
 * acknowledgement goes out with the next segment sent, at a read that ends
 * a write of the peer's (`tcp_cleanup_rbuf`), at the batch's end in quick
 * mode or past `rcv_mss`, or at its deadline, TCP_DELACK_TICKS after the
 * first segment held (`tcp_delack_expire`).
 */
static int tcp_delack_hold(struct tcb *tcb, const uint8_t *data, uint32_t expected, int had_ooo,
                           uint16_t payload_len)
{
    uint8_t flags = data[13];
    uint32_t seq = read32be(data + 4), owed;

    tcb->rcv_wnd = tcp_rcv_window(tcb);
    tcb->ack_sched = 1;
    if (payload_len == 0 || tcb->rcv_nxt == expected) {
        return 0;
    }
    tcp_delack_note(tcb, flags, (uint8_t)((data[12] >> 4) * 4u), payload_len);
    if ((flags & TCP_FIN) || seq != expected || tcb->rcv_nxt != expected + payload_len || had_ooo ||
        tcb->ooo_count != 0) {
        return 0;
    }
    owed = tcb->rcv_nxt - tcb->rcv_acked;
    if (tcp_ack_batch && tcp_batch_depth > 0) {
        if (owed > TCP_BATCH_ACK_MAX) {
            return 0;
        }
        if (!tcb->batch_owed) {
            tcb->batch_owed = 1;
            tcb->batch_next = tcp_batch_list;
            tcp_batch_list = tcb;
        }
    } else if (!tcp_delack || owed > tcb->rcv_mss || (tcb->ack_quick && !tcb->pingpong)) {
        return 0;
    }
    if (tcb->delack_due == 0) {
        tcb->delack_due = tcp_due(TCP_DELACK_TICKS);
    }
    return 1;
}

void tcp_batch_begin(void)
{
    uint32_t flags = net_lock();

    tcp_batch_depth++;
    net_unlock(flags);
}

void tcp_batch_end(void)
{
    uint32_t flags = net_lock();
    struct tcb *t;

    if (tcp_batch_depth > 0 && --tcp_batch_depth == 0) {
        while ((t = tcp_batch_list) != NULL) {
            tcp_batch_list = t->batch_next;
            t->batch_next = NULL;
            t->batch_owed = 0;
            if (t->rcv_nxt == t->rcv_acked || t->state == TCP_CLOSED || t->state == TCP_LISTEN ||
                t->state == TCP_SYN_SENT) {
                continue;
            }
            if (tcp_delack && t->rcv_nxt - t->rcv_acked <= t->rcv_mss &&
                !(t->ack_quick && !t->pingpong)) {
                continue; /* its deadline was set when the data came */
            }
            if (t->state != TCP_TIME_WAIT) {
                t->rcv_wnd = tcp_rcv_window(t); /* TIME_WAIT keeps its own */
            }
            tcp_send_flags(t, TCP_ACK);
        }
    }
    net_unlock(flags);
}

/**
 * Inside a batch, acknowledge what `tcb` holds before it takes the segment
 * of `len` bytes at `seq` when the segment's bytes past `rcv_nxt`, which
 * may follow some it repeats, would carry the unacknowledged data past
 * TCP_BATCH_ACK_MAX, as Linux's GRO sends up what it merged
 * before a segment that would make it too long.
 */
static void tcp_batch_flush(struct tcb *tcb, uint32_t seq, uint16_t len)
{
    uint32_t fresh = seq + len - tcb->rcv_nxt;

    if (tcp_ack_batch && tcp_batch_depth > 0 && tcb->batch_owed &&
        (int32_t)(seq - tcb->rcv_nxt) <= 0 && (int32_t)fresh > 0 &&
        tcb->rcv_nxt - tcb->rcv_acked + fresh > TCP_BATCH_ACK_MAX) {
        tcb->rcv_wnd = tcp_rcv_window(tcb);
        tcp_send_flags(tcb, TCP_ACK);
    }
}

/**
 * Take the peer's FIN at `fin_seq` on `tcb` in ESTABLISHED or FIN_WAIT_1
 * when it is the next in order and the first, entering CLOSE_WAIT or
 * CLOSING and telling the socket.
 */
static void tcp_take_peer_fin(struct tcb *tcb, uint32_t fin_seq)
{
    if (tcb->fin_received || fin_seq != tcb->rcv_nxt) {
        return;
    }
    tcb->rcv_nxt++;
    tcb->fin_received = 1;
    tcb->state = (tcb->state == TCP_FIN_WAIT_1) ? TCP_CLOSING : TCP_CLOSE_WAIT;
    socket_tcp_fin(tcb->sock);
}

/**
 * Take the segment `data` of `len` bytes, whose header is `hdr_len` bytes
 * and whose window field gives `window`, on the connection `tcb` in
 * ESTABLISHED, which `tcp_rx_check` let on, or which the segment brought
 * there from SYN_RCVD, as Linux's tcp_rcv_state_process takes the data of
 * the acknowledgement that ends the handshake (`tcp_rx_addr` says how).
 */
static void tcp_rx_established(struct tcb *tcb, const uint8_t *data, uint16_t hdr_len, uint16_t len,
                               uint32_t window)
{
    uint8_t flags = data[13];
    uint32_t seq = read32be(data + 4), ack_num = read32be(data + 8);
    const uint8_t *payload = data + hdr_len;
    uint16_t payload_len = (uint16_t)(len - hdr_len);
    uint32_t expected = tcb->rcv_nxt;
    int needs_ack, queued = 1, had_ooo = tcb->ooo_count != 0;

    if (flags & TCP_ACK) {
        if ((int32_t)(ack_num - tcb->snd_una) > 0 && (int32_t)(ack_num - tcb->snd_nxt) <= 0) {
            tcp_take_ack(tcb, ack_num, window);
        } else if (ack_num == tcb->snd_una && payload_len == 0 && window == tcb->snd_wnd &&
                   !(flags & (TCP_SYN | TCP_FIN)) && tcb->send_head != tcb->send_ack) {
            if (tcb->recovering == TCP_RECOVER_FAST) {
                tcb->cwnd += tcb->snd_mss;
                tcp_pump(tcb);
            } else if (tcb->recovering == 0) {
                if (tcb->dup_acks < TCP_DUP_ACKS)
                    tcb->dup_acks++;
                if (tcb->dup_acks == TCP_DUP_ACKS && (int32_t)(tcb->snd_una - tcb->recover) > 0) {
                    tcp_enter_fast_recovery(tcb);
                }
            }
        } else if (ack_num == tcb->snd_una && window != tcb->snd_wnd) {
            tcp_take_wnd(tcb, window);
            tcp_arm_zwp(tcb);
            tcp_pump(tcb);
        }
    }

    if (tcp_rx_closed_data(tcb, flags, seq, payload_len, ack_num)) {
        return;
    }
    tcp_rx_resent(tcb, flags, seq, payload_len);
    /* a FIN past a hole too, as Linux's tcp_data_queue_ofo acknowledges */
    needs_ack = payload_len > 0 || (flags & TCP_FIN);
    if (payload_len > 0) {
        tcp_batch_flush(tcb, seq, payload_len);
        queued = tcp_take_data(tcb, seq, payload, payload_len, flags & TCP_FIN);
    }

    if ((flags & TCP_FIN) && queued) {
        tcp_take_peer_fin(tcb, seq + payload_len);
    }

    if (needs_ack) {
        if (tcp_delack_hold(tcb, data, expected, had_ooo, payload_len)) {
            return;
        }
        tcp_send_flags(tcb, TCP_ACK);
    }
}

/**
 * Whether a segment that starts at `seq` and ends before `end` is inside
 * the window the SYN-ACK of the passive block `tcb` offered, as Linux's
 * tcp_check_req judges it with the tcp_in_window of tcp_minisocks.c: it
 * starts at rcv_nxt, or runs past rcv_nxt and starts before the right edge
 * (`rcv_adv`), or is empty at that edge.
 */
static int tcp_syn_rcvd_in_window(const struct tcb *tcb, uint32_t seq, uint32_t end)
{
    if (seq == tcb->rcv_nxt) {
        return 1;
    }
    if ((int32_t)(end - tcb->rcv_nxt) > 0 && (int32_t)(seq - tcb->rcv_adv) < 0) {
        return 1;
    }
    return seq == tcb->rcv_adv && seq == end;
}

/**
 * Send the SYN-ACK of the passive block `tcb` again at once, for the SYN
 * the peer sent again, when `tcp_oow_limited` allows, counted in
 * TCPACKSkippedSynRecv when not, as Linux's tcp_check_req answers a pure
 * resent SYN with inet_rtx_syn_ack: counted in RetransSegs, and the
 * retransmission timer, when it runs, starts its timeout again.
 */
static void tcp_resend_syn_ack(struct tcb *tcb)
{
    if (tcp_oow_limited(tcb, &net_mib.tcp_ack_skipped_syn_recv)) {
        return;
    }
    net_mib.tcp_retrans_segs += tcp_emit_flags(tcb, TCP_SYN | TCP_ACK, tcb->iss) == 0;
    if (tcb->rto_due != 0) {
        tcb->rto_due = tcp_due(tcb->rto);
    }
}

/**
 * Judge a segment of `flags`, not a reset, that starts at `seq`, ends
 * before `end`, its SYN and FIN counted, and acknowledges `ack_num`, for
 * the block `tcb` in SYN_RCVD, before it may end the handshake.
 *
 * A passive block judges it as Linux's tcp_check_req, in its order: a SYN
 * sent again at the peer's first number with no ACK is answered with the
 * SYN-ACK (`tcp_resend_syn_ack`); an ACK of anything but snd_nxt is
 * answered with a reset at that number, as the listener answers it, and
 * the block stays; a segment outside the window offered
 * (`tcp_syn_rcvd_in_window`) is answered with an acknowledgement, which a
 * segment with neither data nor a FIN, or with a SYN, gets at most once in
 * TCP_INVALID_RATELIMIT, counted in TCPACKSkippedSynRecv when held back,
 * and one with data or a FIN always gets (tcp_minisocks.c:790-804,
 * tcp_input.c:3781-3784); a SYN at any other number resets the handshake,
 * which frees the block with its socket and counts in AttemptFails; and
 * one at the peer's first number with the right ACK draws the challenge
 * Linux's new child socket answers it with (TCPSYNChallenge).
 *
 * The block of a simultaneous open judges it as Linux's
 * tcp_rcv_state_process, in tcp_validate_incoming's order and then
 * tcp_ack's (tcp_input.c:6031-6114, :3988-4009, :6867-6878): a segment
 * with neither ACK nor SYN is dropped; a SYN draws a challenge, inside the
 * receive window or outside it, but for the peer's SYN-ACK at its first
 * number, with no data or FIN, that acknowledges snd_nxt; one outside the
 * receive window draws a duplicate acknowledgement (`tcp_dupack`); and an
 * acknowledgement outside [snd_una, snd_nxt] is answered with a reset. One
 * of snd_una, which leaves this side's SYN unacknowledged, ends the
 * handshake as one of snd_nxt does (`tcp_handshake_done`).
 *
 * A segment with no ACK that gets this far is dropped.
 *
 * @return 1 when the segment acknowledges what it must and goes on; 0 when
 * it was answered or dropped here, after which `tcb` may be gone.
 */
static int tcp_rx_syn_rcvd(struct tcb *tcb, uint8_t flags, uint32_t seq, uint32_t end,
                           uint32_t ack_num)
{
    if (tcb->passive) {
        if (seq == tcb->irs && (flags & (TCP_SYN | TCP_ACK)) == TCP_SYN) {
            tcp_resend_syn_ack(tcb);
            return 0;
        }
        if ((flags & TCP_ACK) && ack_num != tcb->snd_nxt) {
            tcp_send_rst_unmatched(tcb->ifindex, tcb->local_ip, tcb->remote_ip, tcb->local_port,
                                   tcb->remote_port, flags, seq, ack_num, end - seq);
            return 0;
        }
        if (!tcp_syn_rcvd_in_window(tcb, seq, end)) {
            if ((end != seq && !(flags & TCP_SYN)) ||
                !tcp_oow_limited(tcb, &net_mib.tcp_ack_skipped_syn_recv)) {
                tcp_send_flags(tcb, TCP_ACK);
            }
            return 0;
        }
        if (flags & TCP_SYN) {
            if (seq != tcb->irs) {
                tcp_send_rst_unmatched(tcb->ifindex, tcb->local_ip, tcb->remote_ip, tcb->local_port,
                                       tcb->remote_port, flags, seq, ack_num, end - seq);
                tcp_reset(tcb);
                return 0;
            }
            net_mib.tcp_syn_challenge++;
            tcp_challenge_ack(tcb);
            return 0;
        }
        return (flags & TCP_ACK) != 0;
    }

    if (!(flags & (TCP_ACK | TCP_SYN))) {
        return 0;
    }
    if ((flags & TCP_SYN) &&
        (!(flags & TCP_ACK) || end != seq + 1u || end != tcb->rcv_nxt || ack_num != tcb->snd_nxt)) {
        net_mib.tcp_syn_challenge++;
        tcp_challenge_ack(tcb);
        return 0;
    }
    if (!tcp_in_window(tcb, seq, end)) {
        tcp_dupack(tcb, seq, end);
        return 0;
    }
    if (!(flags & TCP_ACK)) {
        return 0;
    }
    if ((int32_t)(ack_num - tcb->snd_una) < 0 || (int32_t)(ack_num - tcb->snd_nxt) > 0) {
        tcp_send_rst_unmatched(tcb->ifindex, tcb->local_ip, tcb->remote_ip, tcb->local_port,
                               tcb->remote_port, flags, seq, ack_num, end - seq);
        return 0;
    }
    return 1;
}

/**
 * A segment shorter than the header, longer than TCP_SEG_MAX, with a bad
 * checksum, or with a header length below 20 bytes or past its end is
 * dropped. The checksum is computed against the address the segment was sent
 * to, which on a machine with two cards is not always the first card's.
 *
 * A reset goes to `tcp_rx_reset`, which takes it only at the next expected
 * sequence number or the few others `tcp_rst_verdict` allows, or in
 * SYN_SENT only when it acknowledges the SYN. After the handshake and before
 * TIME_WAIT, `tcp_rx_check` drops a segment without ACK, a SYN, one
 * outside the receive window and one whose acknowledgement is refused,
 * before anything in it is taken, answering each as Linux does: a peer's
 * keepalive probe, below the next expected sequence number, is so answered
 * with a duplicate acknowledgement (RFC 9293, 3.10.7.4). A segment that
 * goes on answers the keepalive's and the zero-window probes when it
 * acknowledges something inside [snd_una, snd_nxt] (`tcp_rx_answers`).
 *
 * A listener takes only a SYN. When its accept backlog is full, the SYN is
 * answered with a reset before anything is allocated, and only the first such
 * refusal is logged. When free memory is inside the reserve, the SYN is
 * dropped, since the allocation would take from what root's login needs, and
 * the peer sends the SYN again. When no control block or socket can be had,
 * the SYN is answered with a reset: a dropped SYN would leave the peer
 * retransmitting and then reporting a timeout, as if the machine were not
 * there. The new connection is on the address the SYN was sent to, since a
 * listener answers whatever address the machine has, and a machine with two
 * cards has two. Its socket is owned by the listener's owner, since a
 * segment received has no calling process, and takes the listener's
 * keepalive and TCP_USER_TIMEOUT, as on Linux.
 *
 * Simultaneous open (RFC 793) is handled in SYN_SENT: both ends sent a SYN and
 * neither has seen an ACK, so this end answers with a SYN-ACK and waits in
 * SYN_RCVD for the peer's. `snd_nxt` is rewound to `iss` first. The first SYN
 * left `snd_nxt` at `iss + 1`, the send emits at `snd_nxt` and takes its
 * retransmit sequence from it, and the peer's answering SYN-ACK acknowledges
 * `iss + 1`; a SYN-ACK sent from `iss + 1` would be acknowledged by nothing,
 * and `snd_una`, `snd_nxt` and the retransmit sequence would disagree.
 *
 * That arm is why SYN_RCVD dispatches on `passive`. A passive block was made by
 * a listener and no descriptor owns its socket until `accept` hands one over,
 * so its completion goes on the listener's queue and a reset frees both. A
 * listener's close frees the passive blocks still in their handshake, with
 * their sockets (`tcp_free_half_opens`), so no completion finds its listener
 * gone. An active opener's socket belongs to a descriptor already, so its
 * completion wakes that `connect` (there is no listener to queue it on) and a
 * reset is a refused connect, leaving the socket for its descriptor to close.
 * Either kind judges the segments of its handshake first (`tcp_rx_syn_rcvd`),
 * and the ACK that ends it goes on to ESTABLISHED's arm with its data and
 * FIN, as Linux's tcp_rcv_state_process takes them after the handshake: a
 * peer whose pure ACK was lost, or one that held the ACK back for its first
 * data, would otherwise wait for its retransmission.
 *
 * In ESTABLISHED (`tcp_rx_established`), an acknowledgement of new data goes
 * to `tcp_take_ack`. A duplicate (RFC 5681, section 2) acknowledges
 * `snd_una` again, with no data, no SYN or FIN and an unchanged window,
 * while data is outstanding: the peer
 * took a segment past a hole. Three in a row mean the hole is a loss and begin
 * a fast recovery, unless they repeat the acknowledgement that ended the last
 * one; each one during the recovery means a segment has left the network and
 * adds one to the congestion window. An ACK of `snd_una` that only changes the
 * window updates it; an older one changes nothing, as Linux's tcp_ack takes it.
 * In-order data advances `rcv_nxt` by what the receive ring took, so a
 * truncated tail is not acknowledged and is sent again; data past a hole is
 * kept by `tcp_take_data` and acknowledged with the byte the hole starts at,
 * which the peer's duplicate count turns into a resend. A FIN is taken once and
 * only in order, which holds only when its segment's data was taken whole, so a
 * FIN on a truncated segment does not signal EOF before its data is delivered.
 * If the acknowledgement in the same segment let `tcp_pump` send this side's
 * FIN, the peer's FIN leads to CLOSING instead of CLOSE_WAIT, the state that
 * expects the ACK of this side's FIN.
 *
 * The FIN_WAIT states follow `tcp_close` or `tcp_shutdown`. After a
 * close, which detaches the socket, or a shutdown of the receiving half
 * too, data or a FIN past rcv_nxt, which no program will read, ends the
 * connection with a reset (`tcp_rx_closed_data`), in ESTABLISHED too while
 * the FIN waits behind queued data; data and a FIN sent again are
 * acknowledged, and a FIN in order is taken. After a shutdown of the
 * sending half alone the socket reads the data and the FIN
 * (`tcp_take_fin`).
 *
 * FIN_WAIT_1 takes an ACK only inside (snd_una, snd_nxt], since a higher one
 * would move snd_una past what was sent, and moves to FIN_WAIT_2, starting
 * its limit, when this side's FIN is acknowledged. The peer's FIN is taken
 * only in order, leading to CLOSING from FIN_WAIT_1 and to TIME_WAIT from
 * FIN_WAIT_2; a duplicate or early one is acknowledged again. CLOSING and
 * LAST_ACK end when this side's FIN is acknowledged, take an acknowledgement
 * of the data before it, and acknowledge a FIN or data the peer sent again
 * (`tcp_rx_after_fin`). CLOSE_WAIT takes acknowledgements and window
 * updates as ESTABLISHED does, goes on sending queued data and a pending
 * FIN, and acknowledges a FIN or data the peer sent again because its
 * acknowledgement was lost, which would otherwise hold the peer in
 * FIN_WAIT_1 until this side's FIN. Each such segment, taken whole before,
 * counts in DelayedACKLost (`tcp_rx_resent`); one whose data runs past the
 * peer's FIN ends the connection, in all three states, as an accepted reset
 * does, and is answered with a reset (`tcp_abort_on_data`), counted in
 * TCPAbortOnData. TIME_WAIT takes a segment as `tcp_rx_time_wait` says:
 * it acknowledges a FIN the peer sent again because its acknowledgement
 * was lost and restarts its 2MSL wait, ends at a reset at `rcv_nxt`, and
 * gives a SYN past `rcv_nxt` to the listener, which opens the pair again
 * with an initial sequence number above the old connection's.
 */
static void tcp_rx_addr(net_addr_t src_ip, net_addr_t dst_ip, const uint8_t *data, uint16_t len)
{
    uint16_t src_port, dst_port;
    uint32_t seq, ack_num;
    uint8_t flags;
    uint16_t field, hdr_len;
    uint32_t window;
    struct tcb *tcb;
    struct tcb *conn_tcb;
    struct socket *conn_sock;
    struct socket *listen_sock;
    uint32_t tw_isn = 0;
    int reopened = 0;

    bkl_assert_held();
    hdr_len = tcp_seg_check(src_ip, dst_ip, data, len);
    if (hdr_len == 0) {
        return;
    }

    src_port = read16be(data + 0);
    dst_port = read16be(data + 2);
    seq = read32be(data + 4);
    ack_num = read32be(data + 8);
    flags = data[13];
    field = read16be(data + 14);

    tcb = tcp_find(dst_ip, dst_port, src_ip, src_port);
    if (!tcb) {
        if (!(flags & TCP_RST)) {
            uint32_t occupied = (uint32_t)(len - hdr_len);

            if (flags & TCP_SYN)
                occupied++;
            if (flags & TCP_FIN)
                occupied++;
            tcp_send_rst_unmatched(0, dst_ip, src_ip, dst_port, src_port, flags, seq, ack_num,
                                   occupied);
        }
        return;
    }

    if (flags & TCP_RST) {
        tcp_rx_reset(tcb, seq,
                     seq + (uint32_t)(len - hdr_len) + ((flags & TCP_SYN) != 0) +
                         ((flags & TCP_FIN) != 0),
                     ack_num, flags);
        return;
    }
    if (tcb->state == TCP_TIME_WAIT) {
        if (!tcp_rx_time_wait(tcb, flags, seq,
                              seq + (uint32_t)(len - hdr_len) + ((flags & TCP_SYN) != 0) +
                                  ((flags & TCP_FIN) != 0)) ||
            !(tcb = tcp_time_wait_reopen(tcb, dst_ip, dst_port, &tw_isn))) {
            return;
        }
        reopened = 1;
    }
    window = (flags & TCP_SYN) ? field : (uint32_t)field << tcb->snd_wscale;
    if (tcp_synced(tcb) && !tcp_rx_check(tcb, flags, seq, (uint32_t)(len - hdr_len), ack_num)) {
        return;
    }
    tcp_rx_answers(tcb, flags, ack_num);

    switch (tcb->state) {
    case TCP_LISTEN:
        if (!(flags & TCP_SYN))
            return;

        if (tcb->sock &&
            ((tcb->sock->accept_tail + 1) & (SOCK_ACCEPT_BACKLOG - 1)) == tcb->sock->accept_head) {
            if (!backlog_refusal_logged) {
                backlog_refusal_logged = 1;
                klog(KLOG_WARN, "tcp",
                     "accept: the backlog of port %d is full and the connection is refused; "
                     "later refusals are not recorded",
                     (int)dst_port);
            }
            tcp_send_rst_unmatched(tcb->ifindex, dst_ip, src_ip, dst_port, src_port, flags, seq,
                                   ack_num, 1);
            return;
        }
        if (frame_free_count() <= mem_reserve_frames()) {
            return;
        }
        conn_tcb = tcb_alloc();
        if (conn_tcb && tcp_ring_alloc(conn_tcb) != 0) {
            tcb_free(conn_tcb);
            conn_tcb = 0;
        }
        if (!conn_tcb) {
            klog(KLOG_WARN, "tcp", "accept: no control block for a connection to port %d",
                 (int)dst_port);
            tcp_send_rst_unmatched(tcb->ifindex, dst_ip, src_ip, dst_port, src_port, flags, seq,
                                   ack_num, 1);
            return;
        }
        conn_sock = socket_alloc(SOCK_STREAM);
        if (!conn_sock) {
            tcb_free(conn_tcb);
            klog(KLOG_WARN, "tcp", "accept: no socket for a connection to port %d", (int)dst_port);
            tcp_send_rst_unmatched(tcb->ifindex, dst_ip, src_ip, dst_port, src_port, flags, seq,
                                   ack_num, 1);
            return;
        }

        conn_sock->uid = tcb->sock ? tcb->sock->uid : tcb->uid;
        if (tcb->sock) {
            conn_sock->keepalive = tcb->sock->keepalive;
            conn_sock->keepidle = tcb->sock->keepidle;
            conn_sock->keepintvl = tcb->sock->keepintvl;
            conn_sock->keepcnt = tcb->sock->keepcnt;
            conn_sock->user_timeout = tcb->sock->user_timeout;
            conn_sock->bound_device = tcb->sock->bound_device;
        }

        conn_tcb->family = tcb->family;
        conn_tcb->ns = tcb->ns;
        conn_tcb->passive = 1;
        conn_tcb->listener = tcb;
        conn_tcb->ifindex = tcb->ifindex;
        conn_tcb->local_ip = dst_ip;
        tcb_take_port(conn_tcb, tcb->local_port, TCP_PORT_FIXED);
        conn_tcb->remote_ip = src_ip;
        conn_tcb->remote_port = src_port;
        tcb_hash_add(conn_tcb);
        conn_tcb->irs = seq;
        conn_tcb->rcv_nxt = seq + 1;
        tcp_take_wnd(conn_tcb, window);
        tcp_take_options(conn_tcb, data, hdr_len);
        conn_tcb->iss = reopened ? tw_isn : tcp_isn(conn_tcb);
        conn_tcb->snd_nxt = conn_tcb->iss;
        conn_tcb->snd_una = conn_tcb->iss;
        conn_tcb->recover = conn_tcb->iss;
        conn_tcb->sock = conn_sock;
        conn_tcb->uid = conn_sock->uid;
        conn_tcb->inode = conn_sock->inode;

        conn_sock->ns = tcb->ns;
        conn_sock->family = tcb->sock ? tcb->sock->family : dst_ip.family;
        conn_sock->v6only = tcb->sock ? tcb->sock->v6only : 0;
        conn_sock->local_ip = dst_ip;
        conn_sock->tcb = conn_tcb;
        conn_sock->local_port = conn_tcb->local_port;
        conn_sock->remote_ip = src_ip;
        conn_sock->remote_port = src_port;

        tcp_send_flags(conn_tcb, TCP_SYN | TCP_ACK);
        conn_tcb->state = TCP_SYN_RCVD;
        net_mib.tcp_passive_opens++;
        break;

    case TCP_SYN_RCVD:
        if (!tcp_rx_syn_rcvd(tcb, flags, seq,
                             seq + (uint32_t)(len - hdr_len) + ((flags & TCP_SYN) != 0) +
                                 ((flags & TCP_FIN) != 0),
                             ack_num)) {
            return;
        }
        tcp_handshake_done(tcb, ack_num, window);
        if (tcb->passive) {
            listen_sock = tcb->listener ? tcb->listener->sock : NULL;
            tcb->listener = NULL;
            if (listen_sock) {
                socket_tcp_accept(listen_sock, tcb->sock);
            }
        } else {
            socket_tcp_connected(tcb->sock);
        }
        tcp_rx_established(tcb, data, hdr_len, len, window);
        break;

    case TCP_SYN_SENT:
        if ((flags & TCP_SYN) && !(flags & TCP_ACK)) {
            tcb->irs = seq;
            tcb->rcv_nxt = seq + 1;
            tcp_take_wnd(tcb, window);
            tcp_take_options(tcb, data, hdr_len);
            tcb->snd_nxt = tcb->iss;
            tcp_send_flags(tcb, TCP_SYN | TCP_ACK);
            tcb->state = TCP_SYN_RCVD;
            return;
        }
        if (!((flags & TCP_SYN) && (flags & TCP_ACK)))
            return;
        if (ack_num != tcb->snd_nxt)
            return;

        tcb->irs = seq;
        tcb->rcv_nxt = seq + 1;
        tcp_handshake_done(tcb, ack_num, window);
        tcp_take_options(tcb, data, hdr_len);
        tcp_send_flags(tcb, TCP_ACK);
        socket_tcp_connected(tcb->sock);
        break;

    case TCP_ESTABLISHED:
        tcp_rx_established(tcb, data, hdr_len, len, window);
        break;

    case TCP_FIN_WAIT_1: {
        uint16_t payload_len = (uint16_t)(len - hdr_len);
        const uint8_t *payload = data + hdr_len;
        int needs_ack = 0, queued = 1;

        if ((flags & TCP_ACK) && (int32_t)(ack_num - tcb->snd_una) > 0 &&
            (int32_t)(ack_num - tcb->snd_nxt) <= 0) {
            tcp_take_ack(tcb, ack_num, window);
            if (ack_num == tcb->snd_nxt) {
                tcb->state = TCP_FIN_WAIT_2;
                tcb->retx_flags = 0;
                tcb->rto_due = 0;
                tcb->timewait_until = TCP_NOW() + TCP_FIN_WAIT2_TICKS;
            }
        }

        if (tcp_rx_closed_data(tcb, flags, seq, payload_len, ack_num)) {
            break;
        }
        tcp_rx_resent(tcb, flags, seq, payload_len);
        if (payload_len > 0) {
            queued = tcp_take_data(tcb, seq, payload, payload_len, flags & TCP_FIN);
            needs_ack = 1;
        }

        if (flags & TCP_FIN) {
            if (queued && seq + payload_len == tcb->rcv_nxt) {
                tcb->rcv_nxt++;
                tcp_take_fin(tcb);
                if (tcb->state == TCP_FIN_WAIT_2) {
                    tcp_enter_time_wait(tcb);
                } else {
                    tcb->state = TCP_CLOSING;
                }
            }
            needs_ack = 1;
        }

        if (needs_ack) {
            tcb->rcv_wnd = tcp_rcv_window(tcb);
            tcp_send_flags(tcb, TCP_ACK);
        }
        break;
    }

    case TCP_FIN_WAIT_2: {
        uint16_t payload_len = (uint16_t)(len - hdr_len);
        const uint8_t *payload = data + hdr_len;
        int needs_ack = 0, queued = 1;

        if (tcp_rx_closed_data(tcb, flags, seq, payload_len, ack_num)) {
            break;
        }
        tcp_rx_resent(tcb, flags, seq, payload_len);
        if (payload_len > 0) {
            queued = tcp_take_data(tcb, seq, payload, payload_len, flags & TCP_FIN);
            needs_ack = 1;
        }
        if (flags & TCP_FIN) {
            if (queued && seq + payload_len == tcb->rcv_nxt) {
                tcb->rcv_nxt++;
                tcp_take_fin(tcb);
                tcp_enter_time_wait(tcb);
            }
            needs_ack = 1;
        }
        if (needs_ack) {
            tcb->rcv_wnd = tcp_rcv_window(tcb);
            tcp_send_flags(tcb, TCP_ACK);
        }
        break;
    }

    case TCP_CLOSING:
        if ((flags & TCP_ACK) && ack_num == tcb->snd_nxt) {
            tcb->retx_flags = 0;
            tcb->rto_due = 0;
            tcp_enter_time_wait(tcb);
        } else {
            if ((flags & TCP_ACK) && (int32_t)(ack_num - tcb->snd_una) > 0 &&
                (int32_t)(ack_num - tcb->snd_nxt) < 0) {
                tcp_take_ack(tcb, ack_num, window);
            }
            (void)tcp_rx_after_fin(tcb, flags, seq, (uint32_t)(len - hdr_len), ack_num);
        }
        break;

    case TCP_LAST_ACK:
        if ((flags & TCP_ACK) && ack_num == tcb->snd_nxt) {
            tcp_closed_or_free(tcb);
        } else {
            if ((flags & TCP_ACK) && (int32_t)(ack_num - tcb->snd_una) > 0 &&
                (int32_t)(ack_num - tcb->snd_nxt) < 0) {
                tcp_take_ack(tcb, ack_num, window);
            }
            (void)tcp_rx_after_fin(tcb, flags, seq, (uint32_t)(len - hdr_len), ack_num);
        }
        break;

    case TCP_CLOSE_WAIT:
        if (tcp_rx_after_fin(tcb, flags, seq, (uint32_t)(len - hdr_len), ack_num)) {
            break;
        }
        if ((flags & TCP_ACK) && (int32_t)(ack_num - tcb->snd_una) > 0 &&
            (int32_t)(ack_num - tcb->snd_nxt) <= 0) {
            tcp_take_ack(tcb, ack_num, window);
            break;
        } else if ((flags & TCP_ACK) && ack_num == tcb->snd_una && window != tcb->snd_wnd) {
            tcp_take_wnd(tcb, window);
            tcp_arm_zwp(tcb);
        }
        tcp_pump(tcb);
        break;

    case TCP_TIME_WAIT: /* `tcp_rx_time_wait` took the segment */
    case TCP_CLOSED:
    default:
        break;
    }
}

void tcp_count(struct tcp_counts *c)
{
    uint32_t flags;
    const struct tcb *t;

    k_memset(c, 0, sizeof(*c));
    flags = net_lock();
    for (t = tcbs; t; t = t->next) {
        c->bytes += (uint32_t)sizeof(*t) + (t->send_buf ? TCP_RING_SIZE : 0) +
                    (t->recv_buf ? t->recv_size : 0) +
                    (t->sent_more ? t->sent_cap * (uint32_t)sizeof(*t->sent_more) : 0);
        if (t->state == TCP_TIME_WAIT) {
            c->tw++;
            continue;
        }
        c->alloc++;
        if (t->state == TCP_CLOSED) {
            continue;
        }
        c->inuse++;
        c->orphan += t->sock == NULL;
        c->estab += t->state == TCP_ESTABLISHED || t->state == TCP_CLOSE_WAIT;
    }
    net_unlock(flags);
}

int tcp_snapshot(struct tcp_row **rows)
{
    uint32_t flags;
    const struct tcb *t;
    struct tcp_row *out;
    int n = 0, room;

    *rows = NULL;
    flags = net_lock();
    for (t = tcbs; t; t = t->next) {
        n++;
    }
    net_unlock(flags);
    if (n == 0) {
        return 0;
    }
    out = kmalloc(sizeof(*out) * (size_t)n);
    if (!out) {
        return -1;
    }
    room = n;
    n = 0;
    flags = net_lock();
    for (t = tcbs; t && n < room; t = t->next) {
        out[n].family = t->family;
        out[n].local_ip = t->local_ip;
        out[n].local_port = t->local_port;
        out[n].remote_ip = t->remote_ip;
        out[n].remote_port = t->remote_port;
        out[n].state = (int)t->state;
        out[n].txq = t->send_tail - t->send_ack;
        out[n].rxq = t->recv_tail - t->recv_head;
        out[n].backlog = 0;
        if (t->state == TCP_LISTEN && t->sock) {
            out[n].rxq =
                (uint32_t)(t->sock->accept_tail - t->sock->accept_head) & (SOCK_ACCEPT_BACKLOG - 1);
            out[n].backlog = SOCK_ACCEPT_BACKLOG - 1;
        }
        out[n].uid = t->uid;
        out[n].inode = t->inode;
        out[n].snd_wnd = t->snd_wnd;
        out[n].cwnd = t->cwnd;
        out[n].in_flight = t->snd_nxt - t->snd_una;
        out[n].unsent = t->send_tail - t->send_head;
        out[n].rto_timer = tcp_left(t->rto_due);
        out[n].zwp_timer = tcp_left(t->zwp_due);
        out[n].tx_refused = t->tx_refused;
        out[n].retransmits = t->retransmits;
        out[n].refused_for = t->refused_since ? TCP_NOW() + 1 - t->refused_since : 0;
        out[n].snd_mss = t->snd_mss;
        out[n].snd_wscale = t->snd_wscale;
        out[n].rcv_wscale = t->rcv_wscale;
        out[n].sack_ok = t->sack_ok;
        out[n].rto_backoff = t->rto_backoff;
        out[n].recovering = t->recovering;
        out[n].rto = t->rto;
        out[n].srtt = t->srtt;
        out[n].rttvar = t->rttvar;
        out[n].srtt_us = t->srtt_us;
        out[n].rttvar_us = t->rttvar_us;
        out[n].ssthresh = t->ssthresh;
        out[n].timewait_left =
            (int32_t)(t->timewait_until - TCP_NOW()) > 0 ? t->timewait_until - TCP_NOW() : 0;
        n++;
    }
    net_unlock(flags);
    *rows = out;
    return n;
}

int tcp_state_to_linux(int state)
{
    switch ((tcp_state_t)state) {
    case TCP_ESTABLISHED:
        return 1;
    case TCP_SYN_SENT:
        return 2;
    case TCP_SYN_RCVD:
        return 3;
    case TCP_FIN_WAIT_1:
        return 4;
    case TCP_FIN_WAIT_2:
        return 5;
    case TCP_TIME_WAIT:
        return 6;
    case TCP_CLOSED:
        return 7;
    case TCP_CLOSE_WAIT:
        return 8;
    case TCP_LAST_ACK:
        return 9;
    case TCP_LISTEN:
        return 10;
    case TCP_CLOSING:
        return 11;
    default:
        return 7;
    }
}

/** Deliver an IPv4 segment to the common TCP state machine. */
void tcp_rx(uint32_t src, uint32_t dst, const uint8_t *data, uint16_t len)
{
    tcp_rx_addr(net_addr4(src), net_addr4(dst), data, len);
}

/** Deliver an IPv6 segment with the receiving link's scope. */
void tcp6_rx(int ifindex, const uint8_t *src, const uint8_t *dst, const uint8_t *data, uint16_t len)
{
    tcp_rx_addr(net_addr6(src, (uint32_t)ifindex), net_addr6(dst, (uint32_t)ifindex), data, len);
}

/** Return a connection's keepalive deadline for the idle timer query.
 * The caller holds the big lock with interrupts off. */
static uint32_t tcp_idle_keepalive(const struct tcb *tcb, uint32_t now)
{
    const struct socket *s = tcb->sock;
    uint32_t idle, intvl, wait = TICK_HZ;

    if (!s || !s->keepalive) {
        return wait;
    }
    if (!tcb->ka_armed) {
        return 1;
    }
    if ((tcb->state != TCP_ESTABLISHED && tcb->state != TCP_CLOSE_WAIT &&
         tcb->state != TCP_FIN_WAIT_2) ||
        tcb->snd_una != tcb->snd_nxt || tcb->send_tail != tcb->send_head) {
        return wait;
    }
    idle = tcp_keep_ticks(s->keepidle ? s->keepidle : tcp_keepalive_time);
    intvl = tcp_keep_ticks(s->keepintvl ? s->keepintvl : tcp_keepalive_intvl);
    IDLE_TIMER_MIN(wait, tcb->ka_probes ? tcb->ka_probe_at + intvl : tcb->ka_since + idle, now);
    return wait;
}

/** Return the next service deadline for the stopped idle tick: the
 * earliest of the blocks before the TIME_WAIT run and of the run's head. */
uint32_t tcp_idle_ticks(void)
{
    struct tcb *tcb;
    uint32_t wait = TICK_HZ, now = TCP_NOW();

    if (tw_owed) {
        return 1;
    }
    if (tw_first) {
        IDLE_TIMER_MIN(wait, tw_first->timewait_until, now);
    }
    for (tcb = tcbs; tcb && tcb != tw_first; tcb = tcb->next) {
        struct socket *s = tcb->sock;

        if (tcb->ack_owed || tcb->send_head != tcb->send_tail) {
            return 1;
        }
        if ((tcb->state == TCP_TIME_WAIT || tcb->state == TCP_FIN_WAIT_2) && !s) {
            IDLE_TIMER_MIN(wait, tcb->timewait_until, now);
        }
        if (!s && (tcb->state == TCP_ESTABLISHED || tcb->state == TCP_CLOSE_WAIT ||
                   tcb->state == TCP_FIN_WAIT_1 || tcb->state == TCP_CLOSING ||
                   tcb->state == TCP_LAST_ACK)) {
            IDLE_TIMER_MIN(wait, tcb->timewait_until, now);
        }
        if (tcb->rto_due != 0) {
            IDLE_TIMER_MIN(wait, tcb->rto_due, now);
        }
        if (tcb->zwp_due != 0) {
            IDLE_TIMER_MIN(wait, tcb->zwp_due, now);
        }
        if (tcb->delack_due != 0) {
            IDLE_TIMER_MIN(wait, tcb->delack_due, now);
        }
        {
            uint32_t ka = tcp_idle_keepalive(tcb, now);

            if (ka < wait) {
                wait = ka;
            }
        }
    }
    return wait;
}
