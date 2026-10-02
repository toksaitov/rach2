/*
 * tcp.c - Transmission Control Protocol state machine.
 *
 * Passive open: tcp_listen sets up a LISTEN TCB; an incoming SYN allocates a
 * connection TCB and socket and replies with SYN-ACK; the ACK completes the
 * handshake and queues the connection for accept().
 *
 * Active open: tcp_connect sends a SYN and moves to SYN_SENT; the SYN-ACK
 * completes the handshake.
 *
 * Reliability: RFC 6298 RTT/RTO with Karn's algorithm; after a timeout,
 * go-back-N retransmission paced by the congestion window; fast retransmit
 * and NewReno recovery from the third duplicate acknowledgement (RFC 5681,
 * RFC 6582); RFC 5681 slow start and congestion avoidance; zero-window
 * probing; and TIME_WAIT. Control segments (SYN, SYN-ACK, FIN) are also
 * retransmitted, and a connection that exhausts its retransmissions is closed
 * with ETIMEDOUT, so a blocked connect(), send() or recv() returns an error
 * instead of hanging.
 *
 * Checksums are verified on input (pseudo-header plus segment). A reset
 * closes a connection only at the next expected sequence number, and one
 * elsewhere in the receive window is answered with an acknowledgement
 * (RFC 5961). An initial sequence number is a clock plus a keyed hash of the
 * connection's addresses and ports (RFC 6528).
 */

#include "../../klog.h"
#include "../../lib/kerrno.h"
#include "tcp.h"
#include "rcvwnd.h"
#include "../socket/socket.h"
#include "../ip/ip.h"
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
#include "../../pit.h"

/* The live control blocks, listeners included, each allocated from the heap
 * by `listen`, by an active open, or for an incoming SYN on the interrupt
 * side, and freed when it is done. The two 64 KiB rings make a block of over
 * 128 KiB. */
static struct tcb *tcbs;

/* TX segment assembly buffer: header + MSS option + one MSS of data. */
static uint8_t tcp_tx_buf[TCP_HDR_SIZE + 4 + TCP_MSS];

/* Checksum scratch: 12-byte pseudo-header + segment. */
static uint8_t tcp_cksum_buf[12 + TCP_HDR_SIZE + 4 + TCP_MSS];

/* Largest TCP segment we accept (fits tcp_cksum_buf). */
#define TCP_SEG_MAX (TCP_HDR_SIZE + 4 + TCP_MSS)

/* The option kinds a SYN's options are read for (RFC 9293). */
#define TCP_OPT_END     0
#define TCP_OPT_NOP     1
#define TCP_OPT_MSS     2
#define TCP_OPT_WS      3
#define TCP_OPT_SACK_OK 4
#define TCP_OPT_SACK    5

/* Ephemeral source port for active opens. */
static uint16_t next_ephemeral = 49152;

extern volatile uint32_t kernel_ticks;

static uint16_t tcp_checksum(uint32_t src_ip, uint32_t dst_ip, const uint8_t *seg,
                             uint16_t seg_len);
static int tcp_emit_flags(struct tcb *tcb, uint8_t flags, uint32_t seq);
static void tcp_send_flags(struct tcb *tcb, uint8_t flags);
static int tcp_output(struct tcb *tcb);
static int tcp_resend(struct tcb *tcb, uint32_t seq, uint32_t most);
static int tcp_recover_send(struct tcb *tcb);
static void tcp_ack_grow(struct tcb *tcb);
static void tcp_pump(struct tcb *tcb);
static void tcp_giveup(struct tcb *tcb);
static struct tcb *tcp_find(uint32_t local_ip, uint16_t local_port, uint32_t remote_ip,
                            uint16_t remote_port);
static struct socket *tcp_find_listen_sock(uint32_t local_ip, uint16_t local_port);
static void tcp_free_half_opens(uint16_t port);

/* The key the initial sequence numbers are hashed under, and whether it has
 * been drawn; `tcp_isn_key` draws it. */
static uint32_t isn_key[4];
static int isn_keyed;

/* Format: RFC 6528's timer counts 4-microsecond units, 2500 to a 10 ms tick. */
#define TCP_ISN_PER_TICK 2500u

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
 * SipHash-2-4 under `isn_key` of the 12 bytes of `tcb`'s local address, remote
 * address, local port and remote port, each little-endian; the low 32 bits.
 */
static uint32_t tcp_isn_hash(const struct tcb *tcb)
{
    uint64_t k0 = TCP_U64(isn_key[1], isn_key[0]);
    uint64_t k1 = TCP_U64(isn_key[3], isn_key[2]);
    uint64_t v0 = k0 ^ TCP_U64(0x736f6d65u, 0x70736575u);
    uint64_t v1 = k1 ^ TCP_U64(0x646f7261u, 0x6e646f6du);
    uint64_t v2 = k0 ^ TCP_U64(0x6c796765u, 0x6e657261u);
    uint64_t v3 = k1 ^ TCP_U64(0x74656462u, 0x79746573u);
    uint64_t m = TCP_U64(tcb->remote_ip, tcb->local_ip);
    uint64_t last = TCP_U64(12u << 24, ((uint32_t)tcb->remote_port << 16) | tcb->local_port);

    v3 ^= m;
    TCP_SIPROUND(v0, v1, v2, v3);
    TCP_SIPROUND(v0, v1, v2, v3);
    v0 ^= m;
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
 * began. The hash is what a host off the path cannot compute, so the time and
 * the ports no longer give the number away.
 */
static uint32_t tcp_isn(const struct tcb *tcb)
{
    return kernel_ticks * TCP_ISN_PER_TICK + tcp_isn_hash(tcb);
}

/**
 * Empty the list of control blocks. The next `listen` or `connect` draws a
 * new key for the initial sequence numbers.
 */
void tcp_init(void)
{
    tcbs = NULL;
    isn_keyed = 0;
}

/**
 * The window to offer, in bytes: the receive ring's free space, clamped to
 * what the 16-bit field carries at the connection's scale. An unscaled
 * ring of 64 KiB is one larger than the field, so a cast alone would offer
 * zero for an empty ring.
 */
static uint32_t tcp_rcv_window(const struct tcb *tcb)
{
    uint32_t free_space = tcb->recv_size - (tcb->recv_tail - tcb->recv_head);
    uint32_t most = (uint32_t)TCP_WINDOW_SIZE << tcb->rcv_wscale;

    return free_space > most ? most : free_space;
}

/* The window field of a segment: the window shifted by the connection's
 * scale, except in a SYN, whose field is never scaled (RFC 7323). */
static uint16_t tcp_wnd_field(const struct tcb *tcb, int syn)
{
    uint32_t field = syn ? tcb->rcv_wnd : tcb->rcv_wnd >> tcb->rcv_wscale;

    return field > TCP_WINDOW_SIZE ? (uint16_t)TCP_WINDOW_SIZE : (uint16_t)field;
}

/**
 * Give a connection its receive ring before its first SYN: TCP_RECV_RING_LARGE
 * when more than TCP_BIG_RING_FRAMES frames are free, TCP_RECV_RING_SMALL
 * otherwise or when the heap has no room for the large one, and the window
 * scale the ring needs, which this side's SYN offers. Returns 0, or -1 when
 * the heap has no room for either.
 */
static int tcp_ring_alloc(struct tcb *tcb)
{
    uint32_t size =
        frame_free_count() > TCP_BIG_RING_FRAMES ? TCP_RECV_RING_LARGE : TCP_RECV_RING_SMALL;
    uint8_t shift = 0;

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

static void tcb_init(struct tcb *tcb)
{
    tcb->in_use = 1;
    tcb->state = TCP_CLOSED;
    tcb->rcv_wnd = TCP_WINDOW_SIZE;
    tcb->ssthresh = 65535;
    tcb->cwnd = TCP_INIT_CWND;
    tcb->snd_mss = TCP_MSS_DEFAULT;
    tcb->rto = TCP_RTO_INIT;
    tcb->sack_offer = 1;
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
 */
static void tcp_take_options(struct tcb *tcb, const uint8_t *data, uint16_t hdr_len)
{
    uint16_t i = TCP_HDR_SIZE;
    uint16_t mss = TCP_MSS_DEFAULT;
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
    tcb->snd_mss = mss;
    tcb->cwnd = (uint32_t)TCP_INIT_SEGS * mss;
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
 * A control block from the heap, initialised and on the list, or NULL when
 * the heap has no room for one. The caller has interrupts off (`net_lock` in
 * a system call), since the receive side walks the same list.
 */
struct tcb *tcb_alloc(void)
{
    struct tcb *tcb = kzalloc(sizeof(*tcb));

    if (!tcb) {
        klog(KLOG_WARN, "tcp", "open: no memory for a control block");
        return NULL;
    }
    tcb_init(tcb);
    tcb->next = tcbs;
    tcbs = tcb;
    return tcb;
}

/**
 * Take a control block off the list and free it; NULL is ignored. The caller
 * has interrupts off and has already detached the block's socket: nothing
 * here clears `sock->tcb`.
 */
void tcb_free(struct tcb *tcb)
{
    struct tcb **link = &tcbs;

    if (!tcb) {
        return;
    }
    while (*link && *link != tcb) {
        link = &(*link)->next;
    }
    if (*link) {
        *link = tcb->next;
    }
    tcb->in_use = 0;
    tcb->state = TCP_CLOSED;
    if (tcb->recv_buf) {
        kfree(tcb->recv_buf);
    }
    kfree(tcb);
}

/**
 * Compute the TCP checksum over a 12-byte pseudo-header (source IP, dest IP,
 * zero, protocol 6, segment length) followed by the segment.
 */
static uint16_t tcp_checksum(uint32_t src_ip, uint32_t dst_ip, const uint8_t *seg, uint16_t seg_len)
{
    write32be(tcp_cksum_buf + 0, src_ip);
    write32be(tcp_cksum_buf + 4, dst_ip);
    tcp_cksum_buf[8] = 0;
    tcp_cksum_buf[9] = 6;
    write16be(tcp_cksum_buf + 10, seg_len);
    k_memcpy(tcp_cksum_buf + 12, seg, seg_len);
    return ip_checksum(tcp_cksum_buf, (uint16_t)(12 + seg_len));
}

/* The index of the held range that has sequence number `seq`, or -1. */
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
 * Write at `opt` the SACK option of an acknowledgement from `tcb`, which
 * holds data past a hole: the ranges of the last segments kept, the newest
 * first, as RFC 2018 asks, and then the other ranges in order, at most
 * TCP_SACK_BLOCKS of them. Returns the bytes written, two NOPs with the
 * option, a multiple of four.
 */
static uint8_t tcp_sack_option(const struct tcb *tcb, uint8_t *opt)
{
    uint8_t n = 0, i, used = 0;
    int r;

    for (i = 0; i < tcb->sack_recent_count && n < TCP_SACK_BLOCKS; i++) {
        r = tcp_ooo_range_of(tcb, tcb->sack_recent[i]);
        if (r >= 0 && !(used & (1u << r))) {
            used |= (uint8_t)(1u << r);
            write32be(opt + 4 + 8 * n, tcb->ooo_start[r]);
            write32be(opt + 8 + 8 * n, tcb->ooo_end[r]);
            n++;
        }
    }
    for (i = 0; i < tcb->ooo_count && n < TCP_SACK_BLOCKS; i++) {
        if (!(used & (1u << i))) {
            write32be(opt + 4 + 8 * n, tcb->ooo_start[i]);
            write32be(opt + 8 + 8 * n, tcb->ooo_end[i]);
            n++;
        }
    }
    opt[0] = TCP_OPT_NOP;
    opt[1] = TCP_OPT_NOP;
    opt[2] = TCP_OPT_SACK;
    opt[3] = (uint8_t)(2 + 8 * n);
    return (uint8_t)(4 + 8 * n);
}

/**
 * Send the segment of `seg_len` bytes in `tcp_tx_buf` from `local_ip` to
 * `remote_ip`,
 * counting it in `net_mib` when IP takes it, and a reset among the resets.
 * The return value is `ip_tx`'s.
 */
static int tcp_ip_tx(uint32_t local_ip, uint32_t remote_ip, uint16_t seg_len)
{
    int rc = ip_tx_from(local_ip, remote_ip, IP_PROTO_TCP, tcp_tx_buf, seg_len, IP_TTL_DEFAULT);

    if (rc == 0) {
        net_mib.tcp_out_segs++;
        if (tcp_tx_buf[13] & TCP_RST) {
            net_mib.tcp_out_rsts++;
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
 * Build and send a segment with the given control flags at the given
 * sequence, without any retransmission bookkeeping or sequence advance.
 * A SYN has the options this side offers, and an acknowledgement sent
 * while data past a hole is held names that data in a SACK option.
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
    write16be(tcp_tx_buf + 14, tcp_wnd_field(tcb, flags & TCP_SYN));
    write16be(tcp_tx_buf + 16, 0);
    write16be(tcp_tx_buf + 18, 0);

    if (flags & TCP_SYN) {
        tcp_tx_buf[20] = TCP_OPT_MSS;
        tcp_tx_buf[21] = 4;
        write16be(tcp_tx_buf + 22, TCP_MSS);
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
    rc = tcp_ip_tx(tcb->local_ip, tcb->remote_ip, seg_len);
    if (rc == 0 && (flags & TCP_ACK)) {
        tcb->ack_owed = 0; /* this segment carried the acknowledgement owed */
    }
    return rc;
}

/**
 * Copy `n` bytes into the ring at `at`, and out of it, in at most two calls.
 *
 * The ring is a power of two and the index wraps through a mask, so a run
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
    }

    if (flags & (TCP_SYN | TCP_FIN)) {
        tcb->retx_flags = flags;
        tcb->retx_seq = seq;
        if (tcb->rto_timer == 0) {
            tcb->rto_timer = tcb->rto;
        }
    }
    if (flags & TCP_SYN)
        tcb->snd_nxt++;
    if (flags & TCP_FIN)
        tcb->snd_nxt++;
}

/**
 * Start the zero-window probe timer when the peer's window is closed over
 * bytes this side still has to send or has sent unacknowledged, and the
 * timer is not running. A window update lost on the way is then found by
 * the probe rather than waited for.
 */
static void tcp_arm_zwp(struct tcb *tcb)
{
    if (tcb->snd_wnd == 0 && tcb->zwp_timer == 0 && tcb->send_tail != tcb->send_ack) {
        tcb->zwp_timer = tcb->rto;
    }
}

/**
 * Transmit data from the send ring. Send as many segments as the advertised
 * and congestion windows allow (minus the bytes already in flight), so a
 * multi-segment response leaves in one burst, not one segment per round
 * trip.
 *
 * A segment the driver would not take (`ip_tx` returns -ENOBUFS) stays in the
 * ring unsent, for the next pump: an acknowledgement, or the tick, which
 * gives up on a connection whose bytes have been refused since
 * TCP_REFUSE_GIVEUP_TICKS ago with nothing sent between. Counted as sent it
 * would wait for a retransmission a second away. Any other refusal is
 * permanent, and the segment is counted as sent so that the retransmission
 * timer and its give-up take the connection down.
 *
 * One round-trip sample runs at a time, on data sent for the first time
 * outside a loss recovery (Karn's algorithm); the acknowledgement that covers
 * the segment ends it.
 *
 * Returns 1 when it stopped on a refusal, 0 when it stopped for any other
 * reason: nothing queued, or a window that allows no more.
 */
static int tcp_output(struct tcb *tcb)
{
    uint32_t avail, in_flight, window, usable, send_len;
    uint16_t seg_len;

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

        send_len = avail;
        if (send_len > tcb->snd_mss)
            send_len = tcb->snd_mss;
        if (send_len > usable)
            send_len = usable;

        if (send_len == 0) {
            tcp_arm_zwp(tcb); /* a closed window is probed, not waited for */
            return 0;
        }

        write16be(tcp_tx_buf + 0, tcb->local_port);
        write16be(tcp_tx_buf + 2, tcb->remote_port);
        write32be(tcp_tx_buf + 4, tcb->snd_nxt);
        write32be(tcp_tx_buf + 8, tcb->rcv_nxt);
        tcp_tx_buf[12] = (uint8_t)((TCP_HDR_SIZE >> 2) << 4);
        tcp_tx_buf[13] = TCP_ACK | TCP_PSH;
        write16be(tcp_tx_buf + 14, tcp_wnd_field(tcb, 0));
        write16be(tcp_tx_buf + 16, 0);
        write16be(tcp_tx_buf + 18, 0);

        ring_get(tcp_tx_buf + TCP_HDR_SIZE, tcb->send_buf, TCP_RING_SIZE, tcb->send_head, send_len);

        seg_len = (uint16_t)(TCP_HDR_SIZE + send_len);
        write16be(tcp_tx_buf + 16,
                  tcp_checksum(tcb->local_ip, tcb->remote_ip, tcp_tx_buf, seg_len));
        if (tcp_ip_tx(tcb->local_ip, tcb->remote_ip, seg_len) == -ENOBUFS) {
            tcb->tx_refused++;
            if (tcb->refused_since == 0) {
                tcb->refused_since = kernel_ticks + 1;
            }
            return 1;
        }
        tcb->refused_since = 0;
        tcb->ack_owed = 0; /* the segment carried the acknowledgement owed */

        tcb->send_head += send_len;
        tcb->snd_nxt += send_len;

        if (tcb->rto_timer == 0) {
            tcb->rto_timer = tcb->rto;
        }
        if (tcb->rtt_start == 0 && !tcb->recovering) {
            tcb->rtt_start = kernel_ticks + 1; /* +1 keeps 0 = no sample */
            tcb->rtt_start_us = pit_now_us();
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

    if ((int32_t)(sent_end - seq) <= 0 || most == 0) {
        return 0;
    }
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
    write16be(tcp_tx_buf + 14, tcp_wnd_field(tcb, 0));
    write16be(tcp_tx_buf + 16, 0);
    write16be(tcp_tx_buf + 18, 0);

    for (i = 0; i < send_len; i++) {
        tcp_tx_buf[TCP_HDR_SIZE + i] = tcb->send_buf[(from + i) & (TCP_RING_SIZE - 1)];
    }

    seg_len = (uint16_t)(TCP_HDR_SIZE + send_len);
    write16be(tcp_tx_buf + 16, tcp_checksum(tcb->local_ip, tcb->remote_ip, tcp_tx_buf, seg_len));
    if (tcp_ip_tx(tcb->local_ip, tcb->remote_ip, seg_len) == -ENOBUFS) {
        tcb->tx_refused++;
        if (tcb->refused_since == 0) {
            tcb->refused_since = kernel_ticks + 1;
        }
        return -1;
    }
    tcb->refused_since = 0;
    tcb->ack_owed = 0; /* the segment carried the acknowledgement owed */
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
 * than one segment per timeout. Returns 1 when the driver refused a segment,
 * which the tick's pump retries.
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
            return 0; /* a zero window is probed by the acknowledgement path */
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
    tcb->rto_timer = tcb->rto;
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

    if (new_ss < 2u * tcb->snd_mss)
        new_ss = 2u * tcb->snd_mss;
    tcb->ssthresh = new_ss;
    tcb->recovering = TCP_RECOVER_FAST;
    tcb->recover = sent_end;
    tcb->rtt_start = 0;
    tcb->fast_pending = tcp_resend(tcb, tcb->snd_una, tcb->snd_mss) < 0;
    tcb->cwnd = new_ss + TCP_DUP_ACKS * tcb->snd_mss;
}

void tcp_path_mtu(uint32_t local_ip, uint32_t remote_ip, uint16_t local_port, uint16_t remote_port,
                  uint32_t seq, uint16_t mtu)
{
    struct tcb *tcb = tcp_find(local_ip, local_port, remote_ip, remote_port);
    uint32_t mss = mtu > 40u ? mtu - 40u : 0;

    if (!tcb || tcb->state < TCP_ESTABLISHED || (int32_t)(seq - tcb->snd_una) < 0 ||
        (int32_t)(seq - tcb->snd_nxt) >= 0) {
        return;
    }
    if (mss < TCP_MSS_DEFAULT) {
        mss = TCP_MSS_DEFAULT;
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
 * Send queued data, and once the send ring has drained, send a FIN that was
 * deferred by close(). This lets close() complete a graceful teardown even
 * when the data did not all fit in one window: incoming ACKs open the window,
 * drive more data out, and finally release the FIN after the last byte.
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

/**
 * Queue up to `len` bytes and return the count accepted. Return zero when the
 * send ring is full, or -1 for a missing connection, negative length, or state
 * other than ESTABLISHED or CLOSE_WAIT.
 *
 * Save and mask interrupts across the connection checks, ring update, and
 * transmit. Retain unacknowledged bytes for retransmission.
 */
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

    if ((tcb->state != TCP_ESTABLISHED && tcb->state != TCP_CLOSE_WAIT) || tcb->fin_pending) {
        net_unlock(flags);
        return -1;
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

/** True if any connection, listener included, occupies this local port. */
static int tcp_local_port_in_use(uint16_t port)
{
    const struct tcb *t;

    for (t = tcbs; t; t = t->next) {
        if (t->local_port == port) {
            return 1;
        }
    }
    return 0;
}

/**
 * The next ephemeral port no control block uses, listeners included, and no
 * other stream socket has bound, or 0 when all 16384 of the range are taken.
 * Caller holds `net_lock`.
 */
static uint16_t ephemeral_port(void)
{
    int tries = 0;
    uint16_t port;

    do {
        port = next_ephemeral++;
        if (next_ephemeral < 49152)
            next_ephemeral = 49152;
        if (++tries > 16384)
            return 0;
    } while (tcp_local_port_in_use(port) || socket_port_bound(port));
    return port;
}

uint16_t tcp_bind_ephemeral(void)
{
    uint32_t flags = net_lock();
    uint16_t port = ephemeral_port();

    net_unlock(flags);
    return port;
}

/**
 * Return whether a LISTEN control block has `port` at an address that meets
 * `ip`: either is any address (0), or both are the same one. Caller holds
 * `net_lock`.
 *
 * Connections in other states do not prevent a new listener on the same
 * local port. This check does not depend on `SO_REUSEADDR`.
 */
static int tcp_port_has_listener(uint32_t ip, uint16_t port)
{
    const struct tcb *t;

    for (t = tcbs; t; t = t->next) {
        if (t->state == TCP_LISTEN && t->local_port == port &&
            (t->local_ip == 0 || ip == 0 || t->local_ip == ip)) {
            return 1;
        }
    }
    return 0;
}

/**
 * Return whether a non-listening control block has the same local port,
 * remote address, and remote port. Caller holds `net_lock`.
 *
 * These three fields are the key used by `tcp_find`. A listener or a
 * connection to a different peer may share the local port: `tcp_find` prefers
 * the exact match and falls back to a listener only when no match exists.
 */
static int tcp_pair_in_use(uint16_t local_port, uint32_t remote_ip, uint16_t remote_port)
{
    const struct tcb *t;

    for (t = tcbs; t; t = t->next) {
        if (t->state != TCP_LISTEN && t->local_port == local_port && t->remote_ip == remote_ip &&
            t->remote_port == remote_port) {
            return 1;
        }
    }
    return 0;
}

/**
 * Answer a segment that matches no connection with the RFC 793 reset.
 *
 * Without this, a SYN to a closed port simply vanishes and the peer's connect
 * hangs until its retransmissions give up; over loopback there is no other
 * host to say no. If the offending segment carried an ACK, the reset claims
 * that acknowledged sequence, the peer's next expected one, and `seg_occupied`
 * is not read; otherwise it acknowledges everything the segment occupied (SYN
 * and FIN each count one) so the peer accepts it. The callers never pass a
 * reset: a reset is not answered with a reset. The same reset answers a
 * handshake that completes after its listener closed.
 */
static void tcp_send_rst_unmatched(uint32_t local_ip, uint32_t remote_ip, uint16_t local_port,
                                   uint16_t remote_port, uint8_t seg_flags, uint32_t seg_seq,
                                   uint32_t seg_ack, uint32_t seg_occupied)
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
    tcp_ip_tx(local_ip, remote_ip, TCP_HDR_SIZE);
}

/**
 * The LISTEN block for a segment to `local_ip`:`local_port`: the one bound to
 * that address, else the one bound to any, else NULL.
 */
static struct tcb *tcp_find_listener(uint32_t local_ip, uint16_t local_port)
{
    struct tcb *t;
    struct tcb *any = NULL;

    for (t = tcbs; t; t = t->next) {
        if (t->state != TCP_LISTEN || t->local_port != local_port)
            continue;
        if (t->local_ip == local_ip)
            return t;
        if (t->local_ip == 0)
            any = t;
    }
    return any;
}

/**
 * Find the TCB for an incoming segment: a block in any state but LISTEN with
 * the same local address and port, remote address and remote port, else the
 * listener for its local address and port.
 */
static struct tcb *tcp_find(uint32_t local_ip, uint16_t local_port, uint32_t remote_ip,
                            uint16_t remote_port)
{
    struct tcb *t;

    for (t = tcbs; t; t = t->next) {
        if (t->state != TCP_LISTEN && t->local_port == local_port && t->local_ip == local_ip &&
            t->remote_ip == remote_ip && t->remote_port == remote_port) {
            return t;
        }
    }
    return tcp_find_listener(local_ip, local_port);
}

static struct socket *tcp_find_listen_sock(uint32_t local_ip, uint16_t local_port)
{
    const struct tcb *t = tcp_find_listener(local_ip, local_port);

    return t ? t->sock : NULL;
}

/**
 * Send a one-byte zero-window probe at snd_una to elicit a window update.
 *
 * The byte probed is the oldest unacknowledged one, at `send_ack`, which the
 * ring has whether it has unsent bytes or only sent ones; an empty ring sends
 * nothing.
 */
static void tcp_send_zwp(struct tcb *tcb)
{
    uint16_t seg_len;

    if (tcb->send_tail == tcb->send_ack)
        return;

    write16be(tcp_tx_buf + 0, tcb->local_port);
    write16be(tcp_tx_buf + 2, tcb->remote_port);
    write32be(tcp_tx_buf + 4, tcb->snd_una);
    write32be(tcp_tx_buf + 8, tcb->rcv_nxt);
    tcp_tx_buf[12] = (uint8_t)((TCP_HDR_SIZE >> 2) << 4);
    tcp_tx_buf[13] = TCP_ACK;
    write16be(tcp_tx_buf + 14, tcp_wnd_field(tcb, 0));
    write16be(tcp_tx_buf + 16, 0);
    write16be(tcp_tx_buf + 18, 0);
    tcp_tx_buf[TCP_HDR_SIZE] = tcb->send_buf[tcb->send_ack & (TCP_RING_SIZE - 1)];
    seg_len = TCP_HDR_SIZE + 1;

    write16be(tcp_tx_buf + 16, tcp_checksum(tcb->local_ip, tcb->remote_ip, tcp_tx_buf, seg_len));
    tcp_ip_tx(tcb->local_ip, tcb->remote_ip, seg_len);
}

/**
 * Abort every connection opened from an address this machine no longer has.
 *
 * Called under `net_lock` when an interface's address changes. A block on an
 * address the machine still has is left alone: on a machine with two cards,
 * changing one card's address says nothing about the connections on the
 * other, and 127/8 is always the machine's. A CLOSED block has nothing to
 * abort, and setting its error again would give a reader the same reset
 * twice. A listener keeps the address it was bound to, as on Linux; one bound
 * to any address answers whatever address the machine has, so a server
 * started before a lease arrived keeps accepting. A passive block in SYN_RCVD
 * has a
 * socket that no descriptor or accept queue refers to, so waking it would
 * leave both to be reclaimed by nothing; it is freed with its socket, as a
 * reset frees it. Any other connection is closed with ENETRESET and its
 * socket woken, or is freed when it has no socket.
 */
void tcp_local_addr_changed(void)
{
    struct tcb *tcb, *next;

    /* A block freed on the way is freed after its successor was read. */
    for (tcb = tcbs; tcb; tcb = next) {
        next = tcb->next;
        if (ip_is_local(tcb->local_ip) || tcb->state == TCP_CLOSED || tcb->state == TCP_LISTEN) {
            continue;
        }
        klog(KLOG_INFO, "tcp", "address changed: aborting a connection opened from the old one");
        if (tcb->state == TCP_SYN_RCVD && tcb->passive) {
            struct socket *s = tcb->sock;

            tcb->sock = NULL;
            if (s) {
                s->tcb = NULL;
                socket_unref(s);
            }
            tcb_free(tcb);
            continue;
        }
        tcb->state = TCP_CLOSED;
        tcb->so_error = ENETRESET;
        tcb->retx_flags = 0;
        tcb->rto_timer = 0;
        tcb->zwp_timer = 0;
        if (tcb->sock) {
            socket_tcp_connected(tcb->sock);
            socket_tcp_fin(tcb->sock);
        } else {
            tcb_free(tcb);
        }
    }
}

/**
 * Abandon a connection that timed out: close it with ETIMEDOUT and wake any
 * blocked call. A passive block in SYN_RCVD, whose handshake the peer never
 * finished, has no descriptor and no place on an accept queue, so it is freed
 * here with its socket; any other block is freed only when it has no socket.
 */
static void tcp_giveup(struct tcb *tcb)
{
    tcp_count_end(tcb->state);
    if (tcb->passive && tcb->state == TCP_SYN_RCVD && tcb->sock) {
        struct socket *sock = tcb->sock;

        klog(KLOG_INFO, "tcp", "accept: a handshake timed out, giving up");
        tcb->sock = NULL;
        sock->tcb = NULL;
        tcb_free(tcb);
        socket_unref(sock);
        return;
    }
    klog(KLOG_INFO, "tcp", "connect: timed out, giving up");
    tcb->state = TCP_CLOSED;
    tcb->so_error = ETIMEDOUT; /* surfaced by SO_ERROR and POLLERR */
    tcb->retx_flags = 0;
    tcb->rto_timer = 0;
    tcb->zwp_timer = 0;
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
 * Retry a connection's pending transmissions. Abandon it when the driver has
 * refused its transmits, with nothing sent between, for more than
 * `TCP_REFUSE_GIVEUP_TICKS`.
 *
 * Return 1 after abandonment, otherwise 0. Abandonment may free the control
 * block.
 */
static int tcp_tick_pump(struct tcb *tcb)
{
    if (tcp_send_more(tcb) == 1 && tcb->refused_since != 0 &&
        kernel_ticks + 1 - tcb->refused_since > TCP_REFUSE_GIVEUP_TICKS) {
        klog(KLOG_WARN, "tcp", "gave up: the driver refused every transmit for %d ticks",
             (int)(kernel_ticks + 1 - tcb->refused_since));
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

    for (tcb = tcbs; tcb; tcb = next) {
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
    for (tcb = tcbs; tcb && pumped < TCP_TICK_PUMPS && index < start; tcb = next) {
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
 * Drive the per-connection timers; called from the timer interrupt once per
 * kernel tick.
 *
 * A TIME_WAIT or FIN_WAIT_2 block is freed when its time is up. The
 * FIN_WAIT_2 limit, TCP_FIN_WAIT2_TICKS, frees a detached block whose peer
 * went away with neither a FIN nor a reset, and which would otherwise never
 * be freed (cf. Linux tcp_fin_timeout).
 *
 * An acknowledgement the interface refused (`ack_owed`) is sent again as a
 * fresh one, carrying the receive state as it now stands, in any state but
 * CLOSED, LISTEN and SYN_SENT, which have nothing to acknowledge. TIME_WAIT
 * is included: a connection enters it by acknowledging the peer's FIN, and a
 * refused acknowledgement would otherwise wait for the peer to send its FIN
 * again, which the block may not outlive.
 *
 * When the retransmission timer expires, the timeout doubles, up to
 * TCP_RTO_MAX, and after TCP_MAX_RETX retransmissions the connection is given
 * up. Data sent and unacknowledged starts a timeout's recovery from the
 * oldest byte, even with a FIN sent behind it, since otherwise a lost data
 * segment is never sent again and the close stalls; the FIN's own
 * retransmission resumes once the data is acknowledged (`send_ack` reaches
 * `send_head`). With no data outstanding, the control segment (SYN, SYN-ACK
 * or FIN) is sent again. The zero-window probe timer sends a probe while the
 * peer's window stays closed and restarts at twice the timeout, up to
 * TCP_RTO_MAX. Last, `tcp_tick_pumps` sends what refused transmits left in
 * the rings.
 */
void tcp_tick(void)
{
    struct tcb *tcb, *next;

    /* A block freed on the way is freed after its successor was read. */
    for (tcb = tcbs; tcb; tcb = next) {
        next = tcb->next;

        if (tcb->state == TCP_TIME_WAIT) {
            if (kernel_ticks >= tcb->timewait_until && !tcb->sock) {
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
        }

        if (tcb->state == TCP_FIN_WAIT_2) {
            if (kernel_ticks >= tcb->timewait_until && !tcb->sock) {
                tcb_free(tcb);
            }
            continue;
        }

        if (tcb->rto_timer > 0) {
            tcb->rto_timer--;
            if (tcb->rto_timer == 0) {
                tcb->rto_backoff++;
                if (tcb->rto_backoff > TCP_MAX_RETX) {
                    tcp_giveup(tcb);
                    continue;
                }

                tcb->rto <<= 1;
                if (tcb->rto > TCP_RTO_MAX)
                    tcb->rto = TCP_RTO_MAX;

                klog(KLOG_DEBUG, "tcp", "retransmit: rto %d, try %d", (unsigned)tcb->rto,
                     (int)tcb->rto_backoff);

                if (tcb->send_head != tcb->send_ack) {
                    tcp_enter_rto_recovery(tcb);
                } else if (tcb->retx_flags) {
                    net_mib.tcp_retrans_segs +=
                        tcp_emit_flags(tcb, tcb->retx_flags, tcb->retx_seq) == 0;
                    tcb->retransmits++;
                    tcb->rto_timer = tcb->rto;
                }
            }
        }

        if (tcb->zwp_timer > 0) {
            tcb->zwp_timer--;
            if (tcb->zwp_timer == 0 && tcb->snd_wnd == 0) {
                tcp_send_zwp(tcb);
                tcb->zwp_timer = tcb->rto << 1;
                if (tcb->zwp_timer > TCP_RTO_MAX)
                    tcb->zwp_timer = TCP_RTO_MAX;
            }
        }
    }
    tcp_tick_pumps();
}

/**
 * Whether a live listener has `port`. Takes `net_lock` itself.
 *
 * For `bind`, which reports the collision where a server expects to hear about
 * it. The check is advisory: `tcp_listen` is what keeps one listener to a
 * port.
 */
int tcp_port_listening(uint32_t ip, uint16_t port)
{
    uint32_t flags = net_lock();
    int held = tcp_port_has_listener(ip, port);

    net_unlock(flags);

    return held;
}

/* The first connection a full backlog refused is recorded, so a log that
 * shows a reset before a session says why; the rest are not, since a
 * listener that falls behind would fill the log. */
static int backlog_refusal_logged;

/**
 * Bind a socket to a port in the LISTEN state for passive opens.
 *
 * The port check lives here and not in `bind`, because `bind` and `listen` are
 * separate syscalls with arbitrary user code between them: `bind(A, P)`,
 * `bind(B, P)`, `listen(A)`, `listen(B)` slips past any check that only `bind`
 * makes, and leaves two LISTEN TCBs on one port for `tcp_find` and
 * `tcp_find_listen_sock` to disagree about. So the scan and the publish are one
 * `net_lock` region, and the check `bind` still makes is advisory.
 *
 * A socket that never bound a port listens on the next ephemeral one, as a
 * `listen` on Linux binds it, and `getsockname` reports the port chosen.
 *
 * @return 0, `-EADDRINUSE` if a listener already has the port or no ephemeral
 *         port is free, or `-ENOBUFS` if the control-block allocation fails.
 */
int tcp_listen(struct socket *sock, uint16_t port)
{
    struct tcb *tcb;
    uint32_t flags;

    flags = net_lock();
    tcp_isn_key();

    if (port == 0) {
        port = ephemeral_port();
    }
    if (port == 0 || tcp_port_has_listener(sock->local_ip, port)) {
        net_unlock(flags);
        return -EADDRINUSE;
    }

    tcb = tcb_alloc();
    if (!tcb) {
        net_unlock(flags);
        return -ENOBUFS;
    }

    tcb->local_ip = sock->local_ip;
    tcb->local_port = port;
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
 * Begin an active open: settle the local port, send the SYN, enter SYN_SENT.
 *
 * Does not block; the handshake completes in tcp_rx and wakes the socket. The
 * port is settled, the block allocated and the SYN sent in one `net_lock`
 * region, and the port is settled before the block exists, so no search sees
 * the caller's own half-built block.
 *
 * A socket that bound a port keeps it. What is refused is only the exact triple
 * `tcp_find` keys on, a local port already paired with this same peer address
 * and port, since two such TCBs would have one connection's segments delivered
 * to the other.
 *
 * A socket without a port takes the next ephemeral one that no block uses,
 * listeners included: over loopback, an ephemeral port equal to that of the
 * listener being dialed would give the looped SYN the same source and
 * destination port, match the caller's own block, and hang the handshake. The
 * search ends within one step more than there are live blocks, or after the
 * range's 16384 ports when every one is in use.
 *
 * The connection is opened from the address the socket bound, or else from
 * the one `ip_source` gives: 127.0.0.1 to 127/8, and otherwise the address of
 * the interface the route to the peer leaves by, the only address the answer
 * can come back to.
 *
 * Return 0 when the open has started, `-EINVAL` from an address in 127/8 to
 * one that is not this machine's, which no card may carry, `-EADDRINUSE` for
 * a duplicate connection key, `-EADDRNOTAVAIL` when no ephemeral port is
 * available, or `-ENOBUFS` when control-block allocation fails.
 */
int tcp_connect(struct socket *sock, uint32_t dst_ip, uint16_t dst_port)
{
    struct tcb *tcb;
    uint32_t flags;
    uint16_t local;
    uint32_t from = sock->local_ip ? sock->local_ip : ip_source(dst_ip);

    if (IP_IS_LOOPBACK(from) && !ip_is_local(dst_ip)) {
        return -EINVAL;
    }
    flags = net_lock();
    tcp_isn_key();

    if (sock->local_port != 0) {
        local = sock->local_port;
        if (tcp_pair_in_use(local, dst_ip, dst_port)) {
            net_unlock(flags);
            return -EADDRINUSE;
        }
    } else {
        local = ephemeral_port();
        if (local == 0) {
            net_unlock(flags);
            return -EADDRNOTAVAIL; /* every ephemeral port is in use */
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

    tcb->local_ip = from;
    tcb->local_port = local;
    tcb->remote_ip = dst_ip;
    tcb->remote_port = dst_port;
    tcb->iss = tcp_isn(tcb);
    tcb->snd_nxt = tcb->iss;
    tcb->snd_una = tcb->iss;
    tcb->recover = tcb->iss;
    tcb->sock = sock;
    tcb->uid = sock->uid;
    tcb->inode = sock->inode;

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
    tcb->retx_flags = 0;
    tcb->rto_timer = 0;
    tcb->zwp_timer = 0;
}

void tcp_shutdown_send(struct socket *sock)
{
    struct tcb *tcb;
    uint32_t flags = net_lock();

    tcb = sock ? sock->tcb : NULL;
    if (tcb && (tcb->state == TCP_ESTABLISHED || tcb->state == TCP_CLOSE_WAIT)) {
        tcb->fin_pending = 1;
        tcp_pump(tcb);
    }
    net_unlock(flags);
}

/**
 * Detach the socket and begin closing the connection.
 *
 * In ESTABLISHED or CLOSE_WAIT, flushes queued data and sends a FIN after the
 * last byte; otherwise frees the TCB. Closing a listener first frees the
 * passive blocks still in their handshake on its port, with their sockets.
 *
 * Detach the socket from the control block in both directions. The caller
 * retains ownership of the socket; shutdown may leave its descriptors open.
 *
 * Save and restore the interrupt state, allowing calls from syscall or
 * interrupt context.
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
        tcp_free_half_opens(tcb->local_port);
    }

    switch (tcb->state) {
    case TCP_ESTABLISHED:
    case TCP_CLOSE_WAIT:
        /* Flush remaining data, then send the FIN after the last byte. */
        tcb->fin_pending = 1;
        tcp_pump(tcb);
        break;
    case TCP_FIN_WAIT_2:
        tcb->timewait_until = kernel_ticks + TCP_FIN_WAIT2_TICKS;
        break;
    case TCP_FIN_WAIT_1:
    case TCP_CLOSING:
    case TCP_LAST_ACK:
    case TCP_TIME_WAIT:
        /* Half closed by shutdown: the FIN went, and the block finishes
         * the close without its socket, as it would after a close. */
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
 * Report whether a stream socket has a peer (ESTABLISHED or any later state).
 *
 * A peer exists from ESTABLISHED through every closing state; the handshake
 * states (SYN_SENT/RCVD), LISTEN, and CLOSED have no peer to name. The state
 * enum is ordered so ESTABLISHED and later compare >= TCP_ESTABLISHED.
 *
 * `sock->tcb` is read once, as `socket_tcp_recv` reads it: an IRQ-side
 * `tcp_close` detaches the block by NULLing `sock->tcb`, so a second read
 * after the NULL check could find NULL.
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
 * Announce a receive window that reopened because the reader drained the ring.
 *
 * The inbound paths recompute `rcv_wnd` whenever a segment arrives, so the peer
 * learns about a window that shrank. A window grows when the reader advances
 * `recv_head` in syscall context, which sends nothing, so it is announced
 * here; without the announcement a peer that filled the buffer waits for its
 * own persist timer before probing again.
 *
 * An update is sent in the states where the peer may still send data:
 * ESTABLISHED, and FIN_WAIT_1 and FIN_WAIT_2, where a reader that shut down
 * its writing half goes on reading. CLOSE_WAIT is entered on the peer's FIN,
 * after which the peer sends no data. The only segment sent is a bare ACK
 * carrying the new window, and the state does not change.
 *
 * Called from syscall context without `net_lock` held, so it takes the lock
 * itself, the same as `tcp_send`, and holds it from the `sock->tcb` snapshot
 * through the send: an IRQ-side `tcp_close` detaches the TCB by NULLing
 * `sock->tcb`, so a re-read after the check could hand a freed block to
 * `tcp_send_flags`.
 */
void tcp_window_update(struct socket *sock)
{
    struct tcb *tcb;
    uint32_t flags;
    uint32_t wnd;

    if (!sock) {
        return;
    }

    flags = net_lock();
    tcb = sock->tcb;
    if (tcb && (tcb->state == TCP_ESTABLISHED || tcb->state == TCP_FIN_WAIT_1 ||
                tcb->state == TCP_FIN_WAIT_2)) {
        wnd = rcvwnd_update(tcb->rcv_wnd, tcb->recv_tail - tcb->recv_head, tcb->recv_size,
                            (uint32_t)TCP_WINDOW_SIZE << tcb->rcv_wscale, TCP_MSS);
        if (wnd != 0u) {
            tcb->rcv_wnd = wnd;
            tcp_send_flags(tcb, TCP_ACK);
        }
    }
    net_unlock(flags);
}

/**
 * Advance snd_una and send_ack for an in-window ACK in a state where our own
 * FIN may be outstanding. The FIN's sequence is not data in the ring, so
 * send_ack is advanced by the acknowledged data only (capped at the ring's
 * sent bytes); this keeps the retransmit timer from resending
 * already-acknowledged data after the FIN is sent. The caller has already
 * checked snd_una < ack_num <= snd_nxt.
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
    tcb->snd_una = ack_num;
    tcb->dup_acks = 0;
    tcb->fast_pending = 0; /* the hole owed was at the old snd_una */
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
 * the timer, and then whatever can be sent. In a fast recovery that the
 * acknowledgement does not end, the next hole is sent at once and the
 * congestion window is deflated by the bytes acknowledged, plus one segment
 * (RFC 6582). The caller has checked that the acknowledgement is inside
 * (snd_una, snd_nxt].
 */
static void tcp_take_ack(struct tcb *tcb, uint32_t ack_num, uint32_t window)
{
    uint32_t acked = ack_num - tcb->snd_una;
    int was_fast = tcb->recovering == TCP_RECOVER_FAST;

    if (tcb->rtt_start != 0 && (int32_t)(ack_num - tcb->rtt_seq) >= 0) {
        uint32_t rtt = kernel_ticks - (tcb->rtt_start - 1);
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
        tcp_rtt_us(tcb, pit_now_us() - tcb->rtt_start_us);
    }

    tcp_ack_data(tcb, ack_num); /* ends a recovery it reaches */
    tcb->snd_wnd = window;
    tcb->rto_backoff = 0;

    if (was_fast) {
        if (tcb->recovering) {
            tcb->cwnd = (tcb->cwnd > acked ? tcb->cwnd - acked : 0) + tcb->snd_mss;
            tcb->fast_pending = tcp_resend(tcb, tcb->snd_una, tcb->snd_mss) < 0;
        }
    } else {
        tcp_ack_grow(tcb);
    }

    if (tcb->snd_una == tcb->snd_nxt) {
        tcb->rto_timer = 0; /* everything acknowledged */
        tcb->zwp_timer = 0;
    } else {
        tcb->rto_timer = tcb->rto;
    }
    tcp_arm_zwp(tcb);
    tcp_pump(tcb);
    if (tcb->sock) {
        socket_tcp_writable(tcb->sock); /* freed send-ring space */
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
 * Accept `len` bytes of in-order received data and report how many to
 * advance rcv_nxt by. A live socket gets the data delivered (and may take
 * less than `len` when its receive ring is nearly full, so the peer
 * retransmits the rest). Once the socket is detached, which `tcp_close` does
 * before the FIN is even sent, so it happens in ESTABLISHED while queued data
 * drains as well as in the FIN_WAIT states, there is no reader: the data is
 * accepted in full and discarded, which still acknowledges it so the peer
 * stops retransmitting and the stream stays in sequence for the FIN.
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
 * with the ranges they overlap or touch. A range that would be a ninth is
 * not recorded, and its bytes are sent again by the peer.
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

/* Note `seq`, the start of a segment just kept past a hole, as the newest
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
 * Take the data of a segment that starts at `seq` (RFC 9293, section
 * 3.10.7.4). Bytes before rcv_nxt were taken already and are skipped. Data
 * at rcv_nxt is delivered, with the kept ranges it reaches; data past it,
 * which a lost segment left a hole before, is kept in the receive ring, so
 * that the resend of the lost segment makes the whole run readable and the
 * peer, whose acknowledgement then covers it, does not send it again.
 */
static void tcp_take_data(struct tcb *tcb, uint32_t seq, const uint8_t *payload, uint16_t len)
{
    uint32_t skip, n;
    uint16_t took;

    if ((int32_t)(seq - tcb->rcv_nxt) < 0) {
        skip = tcb->rcv_nxt - seq;
        if (skip >= len) {
            return;
        }
        payload += skip;
        len = (uint16_t)(len - skip);
        seq = tcb->rcv_nxt;
    }
    if (seq != tcb->rcv_nxt) {
        if (tcb->sock) {
            took = socket_tcp_hold(tcb->sock, seq - tcb->rcv_nxt, payload, len);
            if (took > 0 && tcp_ooo_add(tcb, seq, seq + took)) {
                tcp_sack_note(tcb, seq);
            }
        }
        return;
    }
    took = tcp_accept_data(tcb, payload, len);
    tcb->rcv_nxt += took;
    while (tcb->ooo_count > 0 && (int32_t)(tcb->ooo_start[0] - tcb->rcv_nxt) <= 0) {
        if ((int32_t)(tcb->ooo_end[0] - tcb->rcv_nxt) > 0) {
            n = tcb->ooo_end[0] - tcb->rcv_nxt;
            socket_tcp_release(tcb->sock, n);
            tcb->rcv_nxt += n;
        }
        tcp_ooo_drop_first(tcb);
    }
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
 * Free the passive blocks still in their handshake on `port`, whose listener
 * is closing, with their sockets.
 *
 * Nobody is left to accept them, and a listener opened on the port later
 * must not take a connection that began under this one, with this one's
 * owner; Linux drops them the same way. The peer's final acknowledgement then
 * finds no block: with no listener on the port it is answered with a reset,
 * and a listener opened since ignores a segment that is not a SYN.
 */
static void tcp_free_half_opens(uint16_t port)
{
    struct tcb *t;
    struct tcb *next;

    for (t = tcbs; t; t = next) {
        next = t->next;
        if (t->passive && t->state == TCP_SYN_RCVD && t->local_port == port) {
            tcp_free_half_open(t);
        }
    }
}

/* What `tcp_rst_verdict` allows a reset to do. */
#define TCP_RST_IGNORE    0
#define TCP_RST_CHALLENGE 1
#define TCP_RST_ACCEPT    2

/**
 * Judge a reset for `tcb` by its sequence number, as RFC 5961 (section 3.2)
 * asks.
 *
 * A reset at `rcv_nxt` exactly is accepted. One elsewhere in the receive
 * window is answered with an acknowledgement, which a true peer answers with
 * a reset at `rcv_nxt`, and one outside the window is dropped: a host off the
 * path must then guess the sequence number itself rather than any of the
 * window's values. In SYN_SENT there is no receive window yet, and a reset
 * counts only when it acknowledges the SYN (RFC 793). LISTEN, TIME_WAIT and
 * CLOSED have no connection to reset.
 *
 * @return TCP_RST_ACCEPT, TCP_RST_CHALLENGE or TCP_RST_IGNORE.
 */
static int tcp_rst_verdict(const struct tcb *tcb, uint32_t seq, uint32_t ack_num, uint8_t flags)
{
    switch (tcb->state) {
    case TCP_LISTEN:
    case TCP_TIME_WAIT:
    case TCP_CLOSED:
        return TCP_RST_IGNORE;
    case TCP_SYN_SENT:
        return ((flags & TCP_ACK) && ack_num == tcb->snd_nxt) ? TCP_RST_ACCEPT : TCP_RST_IGNORE;
    case TCP_SYN_RCVD:
    case TCP_ESTABLISHED:
    case TCP_FIN_WAIT_1:
    case TCP_FIN_WAIT_2:
    case TCP_CLOSE_WAIT:
    case TCP_CLOSING:
    case TCP_LAST_ACK:
    default:
        break;
    }
    if (seq == tcb->rcv_nxt) {
        return TCP_RST_ACCEPT;
    }
    if (seq - tcb->rcv_nxt < (uint32_t)tcb->rcv_wnd) {
        return TCP_RST_CHALLENGE;
    }
    return TCP_RST_IGNORE;
}

/**
 * Take a reset for `tcb` after `tcp_rst_verdict` has judged it.
 *
 * A passive block in SYN_RCVD is freed with its socket. Any other connection
 * is closed, with ECONNREFUSED while it is still opening (SYN_SENT, or
 * SYN_RCVD after an active open) and ECONNRESET after, which SO_ERROR and
 * POLLERR report; its socket is woken, or the block is freed when it has no
 * socket. The caller must not use `tcb` afterwards.
 */
static void tcp_rx_reset(struct tcb *tcb, uint32_t seq, uint32_t ack_num, uint8_t flags)
{
    int verdict = tcp_rst_verdict(tcb, seq, ack_num, flags);

    if (verdict == TCP_RST_CHALLENGE) {
        tcp_send_flags(tcb, TCP_ACK);
        return;
    }
    if (verdict != TCP_RST_ACCEPT) {
        return;
    }
    tcp_count_end(tcb->state);
    if (tcb->state == TCP_SYN_RCVD && tcb->passive) {
        tcp_free_half_open(tcb);
        return;
    }
    klog(KLOG_INFO, "tcp", "rx: reset, the connection is closed");
    tcb->so_error =
        (tcb->state == TCP_SYN_SENT || tcb->state == TCP_SYN_RCVD) ? ECONNREFUSED : ECONNRESET;
    tcb->state = TCP_CLOSED;
    tcb->retx_flags = 0;
    tcb->rto_timer = 0;
    tcb->zwp_timer = 0;
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
static uint16_t tcp_seg_check(uint32_t src_ip, uint32_t dst_ip, const uint8_t *data, uint16_t len)
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
 * Process an inbound segment, driving the state machine for its connection.
 *
 * Validates length and checksum, finds the matching connection (or the LISTEN
 * TCB), handles RST, then dispatches on TCP state: completing handshakes,
 * delivering in-order data and acknowledging, and stepping through the closing
 * states. Called with interrupts off from the IP receive path: a card's
 * interrupt, the timer's poll of a card, or a delivery from the loopback ring.
 *
 * A segment shorter than the header, longer than TCP_SEG_MAX, with a bad
 * checksum, or with a header length below 20 bytes or past its end is
 * dropped. The checksum is taken against the address the segment was sent
 * to, which on a machine with two cards is not always the first card's. A
 * segment that matches no connection is answered with a reset, unless it is
 * a reset itself.
 *
 * A reset goes to `tcp_rx_reset`, which takes it only at the next expected
 * sequence number, or in SYN_SENT only when it acknowledges the SYN.
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
 * cards has two.
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
 *
 * In ESTABLISHED, an acknowledgement of new data goes to `tcp_take_ack`. A
 * duplicate (RFC 5681, section 2) acknowledges `snd_una` again, with no data,
 * no SYN or FIN and an unchanged window, while data is outstanding: the peer
 * took a segment past a hole. Three in a row mean the hole is a loss and
 * begin a fast recovery, unless they repeat the acknowledgement that ended
 * the last one; each one during the recovery means a segment has left the
 * network and adds one to the congestion window. An ACK that only changes
 * the window updates it. In-order data advances `rcv_nxt` by what the receive
 * ring took, so a truncated tail is not acknowledged and is sent again; data
 * past a hole is kept by `tcp_take_data` and acknowledged with the byte the
 * hole starts at, which the peer's duplicate count turns into a resend. A FIN
 * is taken once and only in order, which holds only when its segment's data
 * was taken whole, so a FIN on a truncated segment does not signal EOF before
 * its data is delivered. If the acknowledgement in the same segment let
 * `tcp_pump` send this side's FIN, the peer's FIN leads to CLOSING instead of
 * CLOSE_WAIT, the state that expects the ACK of this side's FIN.
 *
 * The FIN_WAIT states follow only `tcp_close`, which detached the socket, so
 * their in-order data is accepted and discarded: acknowledging it stops the
 * peer's retransmissions and keeps the stream in sequence for the peer's FIN.
 * FIN_WAIT_1 takes an ACK only inside (snd_una, snd_nxt], since a higher one
 * would move snd_una past what was sent, and moves to FIN_WAIT_2, starting
 * its limit, when this side's FIN is acknowledged. The peer's FIN is taken
 * only in order, leading to CLOSING from FIN_WAIT_1 and to TIME_WAIT from
 * FIN_WAIT_2; a duplicate or early one is acknowledged again. CLOSING and
 * LAST_ACK end when this side's FIN is acknowledged, take an acknowledgement
 * of the data before it, and acknowledge a retransmitted FIN. CLOSE_WAIT
 * takes acknowledgements and window updates as ESTABLISHED does and goes on
 * sending queued data and a pending FIN. TIME_WAIT acknowledges a FIN the
 * peer sent again because its acknowledgement was lost, and restarts its
 * 2MSL wait.
 */
void tcp_rx(uint32_t src_ip, uint32_t dst_ip, const uint8_t *data, uint16_t len)
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
            tcp_send_rst_unmatched(dst_ip, src_ip, dst_port, src_port, flags, seq, ack_num,
                                   occupied);
        }
        return;
    }

    if (flags & TCP_RST) {
        tcp_rx_reset(tcb, seq, ack_num, flags);
        return;
    }
    window = (flags & TCP_SYN) ? field : (uint32_t)field << tcb->snd_wscale;

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
            tcp_send_rst_unmatched(dst_ip, src_ip, dst_port, src_port, flags, seq, ack_num, 1);
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
            tcp_send_rst_unmatched(dst_ip, src_ip, dst_port, src_port, flags, seq, ack_num, 1);
            return;
        }
        conn_sock = socket_alloc(SOCK_STREAM);
        if (!conn_sock) {
            tcb_free(conn_tcb);
            klog(KLOG_WARN, "tcp", "accept: no socket for a connection to port %d", (int)dst_port);
            tcp_send_rst_unmatched(dst_ip, src_ip, dst_port, src_port, flags, seq, ack_num, 1);
            return;
        }

        /* Receive context has no caller, so the owner is the listener's. */
        conn_sock->uid = tcb->sock ? tcb->sock->uid : tcb->uid;

        conn_tcb->passive = 1;
        conn_tcb->local_ip = dst_ip;
        conn_tcb->local_port = tcb->local_port;
        conn_tcb->remote_ip = src_ip;
        conn_tcb->remote_port = src_port;
        conn_tcb->irs = seq;
        conn_tcb->rcv_nxt = seq + 1;
        conn_tcb->snd_wnd = window;
        tcp_take_options(conn_tcb, data, hdr_len);
        conn_tcb->iss = tcp_isn(conn_tcb);
        conn_tcb->snd_nxt = conn_tcb->iss;
        conn_tcb->snd_una = conn_tcb->iss;
        conn_tcb->recover = conn_tcb->iss;
        conn_tcb->sock = conn_sock;
        conn_tcb->uid = conn_sock->uid;
        conn_tcb->inode = conn_sock->inode;

        conn_sock->tcb = conn_tcb;
        conn_sock->local_port = conn_tcb->local_port;
        conn_sock->remote_ip = src_ip;
        conn_sock->remote_port = src_port;

        tcp_send_flags(conn_tcb, TCP_SYN | TCP_ACK);
        conn_tcb->state = TCP_SYN_RCVD;
        net_mib.tcp_passive_opens++;
        break;

    case TCP_SYN_RCVD:
        if (!(flags & TCP_ACK))
            return;
        if (ack_num != tcb->snd_nxt)
            return;

        tcb->snd_una = ack_num;
        tcb->snd_wnd = window;
        tcb->state = TCP_ESTABLISHED;
        tcb->retx_flags = 0;
        tcb->rto_timer = 0;

        if (tcb->passive) {
            listen_sock = tcp_find_listen_sock(tcb->local_ip, tcb->local_port);
            if (listen_sock) {
                socket_tcp_accept(listen_sock, tcb->sock);
            }
        } else {
            socket_tcp_connected(tcb->sock);
        }
        break;

    case TCP_SYN_SENT:
        if ((flags & TCP_SYN) && !(flags & TCP_ACK)) {
            tcb->irs = seq;
            tcb->rcv_nxt = seq + 1;
            tcb->snd_wnd = window;
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
        tcb->snd_una = ack_num;
        tcb->snd_wnd = window;
        tcp_take_options(tcb, data, hdr_len);
        tcb->state = TCP_ESTABLISHED;
        tcb->retx_flags = 0;
        tcb->rto_timer = 0;

        tcp_send_flags(tcb, TCP_ACK);
        socket_tcp_connected(tcb->sock);
        break;

    case TCP_ESTABLISHED: {
        uint16_t payload_len = (uint16_t)(len - hdr_len);
        const uint8_t *payload = data + hdr_len;
        int needs_ack = 0;

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
                    if (tcb->dup_acks == TCP_DUP_ACKS &&
                        (int32_t)(tcb->snd_una - tcb->recover) > 0) {
                        tcp_enter_fast_recovery(tcb);
                    }
                }
            } else if (window != tcb->snd_wnd) {
                /* Pure window update. */
                tcb->snd_wnd = window;
                tcp_arm_zwp(tcb);
                tcp_pump(tcb);
            }
        }

        if (payload_len > 0) {
            tcp_take_data(tcb, seq, payload, payload_len);
            needs_ack = 1;
        }

        if ((flags & TCP_FIN) && !tcb->fin_received && seq + payload_len == tcb->rcv_nxt) {
            tcb->rcv_nxt++;
            tcb->fin_received = 1;
            tcb->state = (tcb->state == TCP_FIN_WAIT_1) ? TCP_CLOSING : TCP_CLOSE_WAIT;
            socket_tcp_fin(tcb->sock);
            needs_ack = 1;
        }

        if (needs_ack) {
            tcb->rcv_wnd = tcp_rcv_window(tcb);
            tcp_send_flags(tcb, TCP_ACK);
        }
        break;
    }

    case TCP_FIN_WAIT_1: {
        uint16_t payload_len = (uint16_t)(len - hdr_len);
        const uint8_t *payload = data + hdr_len;
        int needs_ack = 0;

        if ((flags & TCP_ACK) && (int32_t)(ack_num - tcb->snd_una) > 0 &&
            (int32_t)(ack_num - tcb->snd_nxt) <= 0) {
            tcp_take_ack(tcb, ack_num, window);
            if (ack_num == tcb->snd_nxt) {
                tcb->state = TCP_FIN_WAIT_2;
                tcb->retx_flags = 0;
                tcb->rto_timer = 0;
                tcb->timewait_until = kernel_ticks + TCP_FIN_WAIT2_TICKS;
            }
        }

        if (payload_len > 0) {
            tcp_take_data(tcb, seq, payload, payload_len);
            needs_ack = 1;
        }

        if (flags & TCP_FIN) {
            if (seq + payload_len == tcb->rcv_nxt) {
                tcb->rcv_nxt++;
                tcp_take_fin(tcb);
                tcb->state = (tcb->state == TCP_FIN_WAIT_2) ? TCP_TIME_WAIT : TCP_CLOSING;
                if (tcb->state == TCP_TIME_WAIT) {
                    tcb->timewait_until = kernel_ticks + TCP_2MSL_TICKS;
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
        int needs_ack = 0;

        if (payload_len > 0) {
            tcp_take_data(tcb, seq, payload, payload_len);
            needs_ack = 1;
        }
        if (flags & TCP_FIN) {
            if (seq + payload_len == tcb->rcv_nxt) {
                tcb->rcv_nxt++;
                tcp_take_fin(tcb);
                tcb->state = TCP_TIME_WAIT;
                tcb->timewait_until = kernel_ticks + TCP_2MSL_TICKS;
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
            tcb->rto_timer = 0;
            tcb->state = TCP_TIME_WAIT;
            tcb->timewait_until = kernel_ticks + TCP_2MSL_TICKS;
        } else {
            if ((flags & TCP_ACK) && (int32_t)(ack_num - tcb->snd_una) > 0 &&
                (int32_t)(ack_num - tcb->snd_nxt) < 0) {
                tcp_take_ack(tcb, ack_num, window); /* a data-only ACK frees the ring */
            }
            if (flags & TCP_FIN) {
                tcp_send_flags(tcb, TCP_ACK); /* re-ACK a retransmitted FIN */
            }
        }
        break;

    case TCP_LAST_ACK:
        if ((flags & TCP_ACK) && ack_num == tcb->snd_nxt) {
            tcp_closed_or_free(tcb);
        } else {
            if ((flags & TCP_ACK) && (int32_t)(ack_num - tcb->snd_una) > 0 &&
                (int32_t)(ack_num - tcb->snd_nxt) < 0) {
                tcp_take_ack(tcb, ack_num, window); /* a data-only ACK frees the ring */
            }
            if (flags & TCP_FIN) {
                tcp_send_flags(tcb, TCP_ACK); /* re-ACK a retransmitted FIN */
            }
        }
        break;

    case TCP_CLOSE_WAIT:
        if ((flags & TCP_ACK) && (int32_t)(ack_num - tcb->snd_una) > 0 &&
            (int32_t)(ack_num - tcb->snd_nxt) <= 0) {
            tcp_take_ack(tcb, ack_num, window);
            break;
        } else if ((flags & TCP_ACK) && window != tcb->snd_wnd) {
            tcb->snd_wnd = window; /* a pure window update reopens a closed window */
            tcp_arm_zwp(tcb);
        }
        tcp_pump(tcb);
        break;

    case TCP_TIME_WAIT:
        if (flags & TCP_FIN) {
            tcp_send_flags(tcb, TCP_ACK);
            tcb->timewait_until = kernel_ticks + TCP_2MSL_TICKS;
        }
        break;

    case TCP_CLOSED:
    default:
        break;
    }
}

/**
 * Copy control blocks into a caller-owned array and return the row count.
 *
 * Count under `net_lock`, allocate outside it, then copy under the lock up to
 * the original count. Newly allocated blocks may be included; growth can
 * exclude older blocks. Return -1 on allocation failure. The caller must free
 * a non-NULL result even when the returned count is zero.
 */
void tcp_count(struct tcp_counts *c)
{
    uint32_t flags;
    const struct tcb *t;

    k_memset(c, 0, sizeof(*c));
    flags = net_lock();
    for (t = tcbs; t; t = t->next) {
        c->bytes += (uint32_t)sizeof(*t) + (t->recv_buf ? t->recv_size : 0);
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
        out[n].rto_timer = t->rto_timer;
        out[n].zwp_timer = t->zwp_timer;
        out[n].tx_refused = t->tx_refused;
        out[n].retransmits = t->retransmits;
        out[n].refused_for = t->refused_since ? kernel_ticks + 1 - t->refused_since : 0;
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
            (int32_t)(t->timewait_until - kernel_ticks) > 0 ? t->timewait_until - kernel_ticks : 0;
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
