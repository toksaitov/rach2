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
 * Reliability: RFC 6298 RTT/RTO with Karn's algorithm, go-back-N data
 * retransmission, RFC 5681 slow-start / congestion avoidance, zero-window
 * probing, and TIME_WAIT. Control segments (SYN, SYN-ACK, FIN) are also
 * retransmitted, and a connection that exhausts its retransmissions is reset
 * so a blocked connect() or recv() returns an error instead of hanging.
 *
 * Checksums are verified on input (pseudo-header plus segment) and a reset in
 * any connected state tears the connection down.
 */

#include "tcp.h"
#include "rcvwnd.h"
#include "../socket/socket.h"
#include "../ip/ip.h"
#include "../core/byteorder.h"
#include "../net.h"
#include "../netconf.h"
#include "../../serial.h"
#include "../../lib/kstring.h"
#include "../../lib/kerrno.h"
#include "../../mm/heap.h"

/*
 * TCB pool, allocated from kernel memory at init rather than living in BSS:
 * the rings make it large (tens of KB), and keeping it out of the kernel image
 * leaves room for the boot ramdisk module just past the kernel.
 */
static struct tcb *tcp_table;

/* TX segment assembly buffer: header + MSS option + one MSS of data. */
static uint8_t tcp_tx_buf[TCP_HDR_SIZE + 4 + TCP_MSS];

/* Checksum scratch: 12-byte pseudo-header + segment. */
static uint8_t tcp_cksum_buf[12 + TCP_HDR_SIZE + 4 + TCP_MSS];

/* Largest TCP segment we accept (fits tcp_cksum_buf). */
#define TCP_SEG_MAX  (TCP_HDR_SIZE + 4 + TCP_MSS)

/* Ephemeral source port for active opens. */
static uint16_t next_ephemeral = 49152;

extern volatile uint32_t kernel_ticks;

static uint16_t tcp_checksum(uint32_t src_ip, uint32_t dst_ip,
                             const uint8_t *seg, uint16_t seg_len);
static void tcp_emit_flags(struct tcb *tcb, uint8_t flags, uint32_t seq);
static void tcp_send_flags(struct tcb *tcb, uint8_t flags);
static void tcp_output(struct tcb *tcb);
static void tcp_retransmit(struct tcb *tcb);
static void tcp_pump(struct tcb *tcb);
static void tcp_giveup(struct tcb *tcb);
static struct tcb    *tcp_find(uint16_t local_port,
                               uint32_t remote_ip, uint16_t remote_port);
static struct socket *tcp_find_listen_sock(uint16_t local_port);

/**
 * Generate an initial sequence number that varies with time and the tuple.
 *
 * Not a security ISN; just enough variation to avoid colliding with sequence
 * numbers from a recently closed connection on the same port pair.
 */
static uint32_t tcp_isn(uint16_t local_port, uint16_t remote_port)
{
    return kernel_ticks * 1103515245UL + 12345UL +
           ((uint32_t)local_port << 16) + remote_port;
}

void tcp_init(void)
{
    tcp_table = (struct tcb *)mem_reserve(sizeof(struct tcb) * MAX_TCP_CONNS,
                                          true);
    if (tcp_table) {
        k_memset(tcp_table, 0, sizeof(struct tcb) * MAX_TCP_CONNS);
    }
}

static void tcb_init(struct tcb *tcb)
{
    k_memset(tcb, 0, sizeof(struct tcb));
    tcb->in_use   = 1;
    tcb->state    = TCP_CLOSED;
    tcb->rcv_wnd  = TCP_WINDOW_SIZE;
    tcb->ssthresh = 65535;
    tcb->cwnd     = TCP_INIT_CWND;
    tcb->rto      = TCP_RTO_INIT;
}

/**
 * Allocate a TCB from the shared pool, reclaiming a closing slot if full.
 *
 * Returns a free, initialized TCB, or NULL if none can be had. When every
 * slot is in use, the soonest-to-expire TIME_WAIT or FIN_WAIT_2 connection is
 * recycled rather than failing the new connection (see the reclaim note below).
 */
struct tcb *tcb_alloc(void)
{
    int i;
    struct tcb *recycle = NULL;

    if (!tcp_table) {
        return NULL;
    }

    for (i = 0; i < MAX_TCP_CONNS; i++) {
        if (!tcp_table[i].in_use) {
            tcb_init(&tcp_table[i]);
            return &tcp_table[i];
        }
        /* Track the closing connection closest to expiring, in case the pool
         * is full and we must reclaim one. TIME_WAIT and FIN_WAIT_2 are both
         * detached and only waiting on the peer, so either is safe to drop;
         * both carry a deadline in timewait_until. */
        if ((tcp_table[i].state == TCP_TIME_WAIT ||
             tcp_table[i].state == TCP_FIN_WAIT_2) &&
            (!recycle ||
             (int32_t)(tcp_table[i].timewait_until - recycle->timewait_until) < 0)) {
            recycle = &tcp_table[i];
        }
    }

    /*
     * No free slot. Reclaim the soonest-to-expire closing connection rather
     * than drop the new one: TIME_WAIT only guards against stale duplicate
     * segments and FIN_WAIT_2 is merely awaiting the peer's FIN, both
     * negligible to abandon on the local QEMU network, and recycling keeps the
     * pool usable under bursty load such as a browser opening many parallel
     * connections.
     */
    if (recycle) {
        tcb_init(recycle);
        return recycle;
    }

    serial_printf("TCP: TCB pool exhausted\n");
    return NULL;
}

void tcb_free(struct tcb *tcb)
{
    if (tcb) {
        tcb->in_use = 0;
        tcb->state  = TCP_CLOSED;
    }
}

/*
 * Compute the TCP checksum over a 12-byte pseudo-header (source IP, dest IP,
 * zero, protocol 6, segment length) followed by the segment.
 */
static uint16_t tcp_checksum(uint32_t src_ip, uint32_t dst_ip,
                             const uint8_t *seg, uint16_t seg_len)
{
    write32be(tcp_cksum_buf + 0, src_ip);
    write32be(tcp_cksum_buf + 4, dst_ip);
    tcp_cksum_buf[8] = 0;
    tcp_cksum_buf[9] = 6;
    write16be(tcp_cksum_buf + 10, seg_len);
    k_memcpy(tcp_cksum_buf + 12, seg, seg_len);
    return ip_checksum(tcp_cksum_buf, (uint16_t)(12 + seg_len));
}

/* Build and send a segment with the given control flags at the given sequence,
 * without any retransmission bookkeeping or sequence advance. */
static void tcp_emit_flags(struct tcb *tcb, uint8_t flags, uint32_t seq)
{
    uint8_t  hdr_len;
    uint16_t seg_len;

    hdr_len = (flags & TCP_SYN) ? 24 : 20;   /* SYN carries an MSS option */

    write16be(tcp_tx_buf + 0,  tcb->local_port);
    write16be(tcp_tx_buf + 2,  tcb->remote_port);
    write32be(tcp_tx_buf + 4,  seq);
    write32be(tcp_tx_buf + 8,  (flags & TCP_ACK) ? tcb->rcv_nxt : 0);
    tcp_tx_buf[12] = (uint8_t)((hdr_len >> 2) << 4);
    tcp_tx_buf[13] = flags;
    write16be(tcp_tx_buf + 14, tcb->rcv_wnd);
    write16be(tcp_tx_buf + 16, 0);
    write16be(tcp_tx_buf + 18, 0);

    if (flags & TCP_SYN) {
        tcp_tx_buf[20] = 2;
        tcp_tx_buf[21] = 4;
        write16be(tcp_tx_buf + 22, TCP_MSS);
    }

    seg_len = hdr_len;
    write16be(tcp_tx_buf + 16,
              tcp_checksum(tcb->local_ip, tcb->remote_ip, tcp_tx_buf, seg_len));
    ip_tx(tcb->remote_ip, IP_PROTO_TCP, tcp_tx_buf, seg_len);
}

/* Send a control segment at snd_nxt. SYN and FIN consume a sequence number and
 * are marked for retransmission until acknowledged. */
static void tcp_send_flags(struct tcb *tcb, uint8_t flags)
{
    uint32_t seq = tcb->snd_nxt;

    tcp_emit_flags(tcb, flags, seq);

    if (flags & (TCP_SYN | TCP_FIN)) {
        tcb->retx_flags = flags;
        tcb->retx_seq   = seq;
        if (tcb->rto_timer == 0) {
            tcb->rto_timer = tcb->rto;
        }
    }
    if (flags & TCP_SYN) tcb->snd_nxt++;
    if (flags & TCP_FIN) tcb->snd_nxt++;
}

/*
 * Transmit data from the send ring. Send as many segments as the advertised
 * and congestion windows allow (minus the bytes already in flight), so a
 * multi-segment response leaves in one burst rather than one segment per round
 * trip.
 */
static void tcp_output(struct tcb *tcb)
{
    uint32_t avail, in_flight, window, usable, send_len, i;
    uint16_t seg_len;

    for (;;) {
        avail = tcb->send_tail - tcb->send_head;
        if (avail == 0) {
            return;
        }

        in_flight = tcb->snd_nxt - tcb->snd_una;

        window = tcb->snd_wnd;
        if (tcb->cwnd < window) {
            window = tcb->cwnd;
        }
        usable = (window > in_flight) ? (window - in_flight) : 0;

        send_len = avail;
        if (send_len > TCP_MSS) send_len = TCP_MSS;
        if (send_len > usable)  send_len = usable;

        if (send_len == 0) {
            /* Window closed: probe if the peer advertised a zero window. */
            if (tcb->snd_wnd == 0 && tcb->zwp_timer == 0) {
                tcb->zwp_timer = tcb->rto;
            }
            return;
        }

        write16be(tcp_tx_buf + 0,  tcb->local_port);
        write16be(tcp_tx_buf + 2,  tcb->remote_port);
        write32be(tcp_tx_buf + 4,  tcb->snd_nxt);
        write32be(tcp_tx_buf + 8,  tcb->rcv_nxt);
        tcp_tx_buf[12] = (uint8_t)((TCP_HDR_SIZE >> 2) << 4);
        tcp_tx_buf[13] = TCP_ACK | TCP_PSH;
        write16be(tcp_tx_buf + 14, tcb->rcv_wnd);
        write16be(tcp_tx_buf + 16, 0);
        write16be(tcp_tx_buf + 18, 0);

        for (i = 0; i < send_len; i++) {
            tcp_tx_buf[TCP_HDR_SIZE + i] =
                tcb->send_buf[(tcb->send_head + i) & (TCP_RING_SIZE - 1)];
        }

        seg_len = (uint16_t)(TCP_HDR_SIZE + send_len);
        write16be(tcp_tx_buf + 16,
                  tcp_checksum(tcb->local_ip, tcb->remote_ip,
                               tcp_tx_buf, seg_len));
        ip_tx(tcb->remote_ip, IP_PROTO_TCP, tcp_tx_buf, seg_len);

        tcb->send_head += send_len;
        tcb->snd_nxt   += send_len;

        if (tcb->rto_timer == 0) {
            tcb->rto_timer = tcb->rto;
            if (tcb->rtt_start == 0) {
                tcb->rtt_start = kernel_ticks + 1; /* +1 keeps 0 = no sample */
            }
        }
    }
}

/*
 * Resend the oldest unacknowledged segment (the MSS at snd_una) after a
 * timeout. Unlike tcp_output this reads from the acknowledged ring position
 * and leaves snd_nxt and send_head untouched, so snd_nxt stays the true
 * high-water mark of transmitted data: an ACK for anything already sent
 * therefore still satisfies the (ack_num <= snd_nxt) window test and advances
 * the connection instead of being rejected. As ACKs arrive, send_ack moves up
 * and tcp_output resumes sending new data from send_head.
 */
static void tcp_retransmit(struct tcb *tcb)
{
    uint32_t data_len, send_len, i;
    uint16_t seg_len;

    /* Bound by the SENT-but-unacknowledged data, [send_ack, send_head): not
     * snd_nxt - snd_una (which also counts an outstanding FIN's sequence) and
     * not send_tail - send_ack (which also counts queued-but-unsent data past
     * snd_nxt). Either would transmit bytes the peer could ACK beyond snd_nxt,
     * wedging the stream. */
    data_len = tcb->send_head - tcb->send_ack;
    if (data_len == 0) {
        return;
    }

    send_len = data_len;
    if (send_len > TCP_MSS) send_len = TCP_MSS;

    write16be(tcp_tx_buf + 0,  tcb->local_port);
    write16be(tcp_tx_buf + 2,  tcb->remote_port);
    write32be(tcp_tx_buf + 4,  tcb->snd_una);
    write32be(tcp_tx_buf + 8,  tcb->rcv_nxt);
    tcp_tx_buf[12] = (uint8_t)((TCP_HDR_SIZE >> 2) << 4);
    tcp_tx_buf[13] = TCP_ACK | TCP_PSH;
    write16be(tcp_tx_buf + 14, tcb->rcv_wnd);
    write16be(tcp_tx_buf + 16, 0);
    write16be(tcp_tx_buf + 18, 0);

    for (i = 0; i < send_len; i++) {
        tcp_tx_buf[TCP_HDR_SIZE + i] =
            tcb->send_buf[(tcb->send_ack + i) & (TCP_RING_SIZE - 1)];
    }

    seg_len = (uint16_t)(TCP_HDR_SIZE + send_len);
    write16be(tcp_tx_buf + 16,
              tcp_checksum(tcb->local_ip, tcb->remote_ip, tcp_tx_buf, seg_len));
    ip_tx(tcb->remote_ip, IP_PROTO_TCP, tcp_tx_buf, seg_len);

    tcb->rto_timer = tcb->rto;
}

/*
 * Send queued data, and once the send ring has drained, send a FIN that was
 * deferred by close(). This lets close() complete a graceful teardown even
 * when the data did not all fit in one window: incoming ACKs open the window,
 * drive more data out, and finally release the FIN after the last byte.
 */
static void tcp_pump(struct tcb *tcb)
{
    tcp_output(tcb);

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
 * Queue up to `len` bytes for transmission and return the count accepted.
 *
 * Copies into the send ring (bounded by free space measured from the
 * acknowledged position) and kicks tcp_output. Only valid in ESTABLISHED or
 * CLOSE_WAIT; returns -1 on a bad socket, a negative length, or a wrong state.
 */
int tcp_send(struct socket *sock, const uint8_t *buf, int len)
{
    struct tcb *tcb;
    uint32_t free_space, copy_len, i, flags;

    if (!sock || !sock->tcb || len < 0) {
        return -1;
    }
    tcb = sock->tcb;

    if (tcb->state != TCP_ESTABLISHED && tcb->state != TCP_CLOSE_WAIT) {
        return -1;
    }

    /* Disable interrupts so an IRQ-context send cannot re-enter the stack and
     * corrupt the shared TX buffers or the ring pointers mid-build. */
    flags = net_lock();

    /* Free space is measured from the acknowledged position so unacked bytes
     * stay in the ring for retransmission. */
    free_space = TCP_RING_SIZE - (tcb->send_tail - tcb->send_ack);
    copy_len   = (uint32_t)len;
    if (copy_len > free_space) copy_len = free_space;

    if (copy_len > 0) {
        for (i = 0; i < copy_len; i++) {
            tcb->send_buf[(tcb->send_tail + i) & (TCP_RING_SIZE - 1)] = buf[i];
        }
        tcb->send_tail += copy_len;
        tcp_output(tcb);
    }

    net_unlock(flags);
    return (int)copy_len;
}

/* True if any connection, listener included, occupies this local port. */
static int tcp_local_port_in_use(uint16_t port)
{
    int i;

    for (i = 0; i < MAX_TCP_CONNS; i++) {
        if (tcp_table[i].in_use && tcp_table[i].local_port == port) {
            return 1;
        }
    }
    return 0;
}

/**
 * Whether a live listener already holds `port`. Caller holds `net_lock`.
 *
 * The scope is `TCP_LISTEN` and not every in-use TCB, deliberately. An accepted
 * connection and a TIME_WAIT remnant both hold a local port in this table, and
 * `setsockopt` is a no-op here, so `SO_REUSEADDR` buys nothing: a scan that
 * counted them would make restarting dropbear or httpd after a session fail at
 * `bind(22)` or `bind(80)` where today it succeeds. What a connecting, closing
 * or TIME_WAIT connection holds is a port against nobody.
 *
 * Answers 0 when the pool was never reserved, since this runs before the
 * allocator that would otherwise catch it.
 */
static int tcp_port_has_listener(uint16_t port)
{
    int i;

    if (!tcp_table) {
        return 0;
    }

    for (i = 0; i < MAX_TCP_CONNS; i++) {
        if (tcp_table[i].in_use && tcp_table[i].state == TCP_LISTEN &&
            tcp_table[i].local_port == port) {
            return 1;
        }
    }
    return 0;
}

/**
 * Whether the exact four-tuple this stack keys on is already taken. Caller
 * holds `net_lock`.
 *
 * `tcp_find` keys on the local port, the remote address and the remote port, so
 * two TCBs sharing all three would have one connection's segments delivered to
 * whichever the scan reached first. Only an identical triple is a conflict: a
 * bound port shared with a listener, or with a connection to a different peer,
 * stays legal, since `tcp_find` returns the exact match and falls back to the
 * listener only when there is none.
 *
 * Answers 0 when the pool was never reserved, for the reason above.
 */
static int tcp_pair_in_use(uint16_t local_port, uint32_t remote_ip, uint16_t remote_port)
{
    int i;

    if (!tcp_table) {
        return 0;
    }

    for (i = 0; i < MAX_TCP_CONNS; i++) {
        if (tcp_table[i].in_use && tcp_table[i].state != TCP_LISTEN &&
            tcp_table[i].local_port == local_port && tcp_table[i].remote_ip == remote_ip &&
            tcp_table[i].remote_port == remote_port) {
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
 * that acknowledged sequence; otherwise it acknowledges everything the
 * segment occupied (SYN and FIN each count one) so the peer accepts it. A
 * reset is never answered with a reset.
 */
static void tcp_send_rst_unmatched(uint32_t remote_ip, uint16_t local_port,
                                   uint16_t remote_port, uint8_t seg_flags,
                                   uint32_t seg_seq, uint32_t seg_ack,
                                   uint32_t seg_occupied)
{
    uint8_t  flags;
    uint32_t seq, ack;

    if (seg_flags & TCP_ACK) {
        flags = TCP_RST;
        seq   = seg_ack;
        ack   = 0;
    } else {
        flags = TCP_RST | TCP_ACK;
        seq   = 0;
        ack   = seg_seq + seg_occupied;
    }

    write16be(tcp_tx_buf + 0,  local_port);
    write16be(tcp_tx_buf + 2,  remote_port);
    write32be(tcp_tx_buf + 4,  seq);
    write32be(tcp_tx_buf + 8,  ack);
    tcp_tx_buf[12] = (uint8_t)((TCP_HDR_SIZE >> 2) << 4);
    tcp_tx_buf[13] = flags;
    write16be(tcp_tx_buf + 14, 0);
    write16be(tcp_tx_buf + 16, 0);
    write16be(tcp_tx_buf + 18, 0);
    write16be(tcp_tx_buf + 16,
              tcp_checksum(net_ip_addr, remote_ip, tcp_tx_buf, TCP_HDR_SIZE));
    ip_tx(remote_ip, IP_PROTO_TCP, tcp_tx_buf, TCP_HDR_SIZE);
}

/* Find the TCB for an incoming segment: an exact tuple match in a non-LISTEN
 * state, else the LISTEN TCB for the port. */
static struct tcb *tcp_find(uint16_t local_port,
                            uint32_t remote_ip, uint16_t remote_port)
{
    int i;
    struct tcb *listen_tcb = NULL;

    for (i = 0; i < MAX_TCP_CONNS; i++) {
        if (!tcp_table[i].in_use) continue;
        if (tcp_table[i].local_port != local_port) continue;

        if (tcp_table[i].state == TCP_LISTEN) {
            listen_tcb = &tcp_table[i];
            continue;
        }
        if (tcp_table[i].remote_ip == remote_ip &&
            tcp_table[i].remote_port == remote_port) {
            return &tcp_table[i];
        }
    }
    return listen_tcb;
}

static struct socket *tcp_find_listen_sock(uint16_t local_port)
{
    int i;

    for (i = 0; i < MAX_TCP_CONNS; i++) {
        if (tcp_table[i].in_use &&
            tcp_table[i].state == TCP_LISTEN &&
            tcp_table[i].local_port == local_port) {
            return tcp_table[i].sock;
        }
    }
    return NULL;
}

/* Send a one-byte zero-window probe at snd_una to elicit a window update. */
static void tcp_send_zwp(struct tcb *tcb)
{
    uint16_t seg_len;

    if (tcb->send_tail == tcb->send_head) return;

    write16be(tcp_tx_buf + 0,  tcb->local_port);
    write16be(tcp_tx_buf + 2,  tcb->remote_port);
    write32be(tcp_tx_buf + 4,  tcb->snd_una);
    write32be(tcp_tx_buf + 8,  tcb->rcv_nxt);
    tcp_tx_buf[12] = (uint8_t)((TCP_HDR_SIZE >> 2) << 4);
    tcp_tx_buf[13] = TCP_ACK;
    write16be(tcp_tx_buf + 14, tcb->rcv_wnd);
    write16be(tcp_tx_buf + 16, 0);
    write16be(tcp_tx_buf + 18, 0);
    tcp_tx_buf[TCP_HDR_SIZE] = tcb->send_buf[tcb->send_ack & (TCP_RING_SIZE - 1)];
    seg_len = TCP_HDR_SIZE + 1;

    write16be(tcp_tx_buf + 16,
              tcp_checksum(tcb->local_ip, tcb->remote_ip, tcp_tx_buf, seg_len));
    ip_tx(tcb->remote_ip, IP_PROTO_TCP, tcp_tx_buf, seg_len);
}

void tcp_local_addr_changed(uint32_t new_ip)
{
    int i;

    if (!tcp_table) {
        return;
    }
    for (i = 0; i < MAX_TCP_CONNS; i++) {
        struct tcb *tcb = &tcp_table[i];

        if (!tcb->in_use || tcb->local_ip == new_ip) {
            continue;
        }
        /* A block that is already closed has nothing to abort, and arming its
           reason again would hand the same reset to a reader twice: the reason
           belongs to the first reader, and this is not a new one. */
        if (tcb->state == TCP_CLOSED) {
            continue;
        }
        /* A listener is not a connection: it is bound to a port and answers
           whatever address this machine has, so it follows the change instead
           of dying with it. A server started before a lease arrived would
           otherwise stop accepting and never know why. */
        if (tcb->state == TCP_LISTEN) {
            tcb->local_ip = new_ip;
            continue;
        }
        serial_printf("tcp: address changed: aborting a connection opened from the old one\n");
        /* A half-open passive block owns a socket nobody has accepted yet and
           no descriptor refers to, so waking it would leave both to be reclaimed
           by nothing. It is released the way a reset releases it. */
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
        tcb->state      = TCP_CLOSED;
        tcb->so_error   = ENETRESET;
        tcb->retx_flags = 0;
        tcb->rto_timer  = 0;
        tcb->zwp_timer  = 0;
        if (tcb->sock) {
            socket_tcp_connected(tcb->sock);
            socket_tcp_fin(tcb->sock);
        } else {
            tcb_free(tcb);
        }
    }
}

/* Abandon a connection that timed out: reset it and wake any blocked
 * syscall. */
static void tcp_giveup(struct tcb *tcb)
{
    serial_printf("TCP: connection timed out, giving up\n");
    tcb->state      = TCP_CLOSED;
    tcb->so_error   = ETIMEDOUT; /* surfaced by SO_ERROR and POLLERR */
    tcb->retx_flags = 0;
    tcb->rto_timer  = 0;
    tcb->zwp_timer  = 0;

    if (tcb->sock) {
        socket_tcp_connected(tcb->sock);
        socket_tcp_fin(tcb->sock);
    } else {
        tcb_free(tcb);
    }
}

/**
 * Drive the per-connection timers, called once per kernel tick.
 *
 * Expires TIME_WAIT and FIN_WAIT_2 slots, runs the RTO timer (backing off and
 * retransmitting, or giving up after TCP_MAX_RETX tries), and runs the
 * zero-window probe timer.
 */
void tcp_tick(void)
{
    int i;
    struct tcb *tcb;

    if (!tcp_table) {
        return;
    }

    for (i = 0; i < MAX_TCP_CONNS; i++) {
        tcb = &tcp_table[i];
        if (!tcb->in_use) continue;

        if (tcb->state == TCP_TIME_WAIT) {
            if (kernel_ticks >= tcb->timewait_until) {
                tcb_free(tcb);
            }
            continue;
        }

        /*
         * FIN_WAIT_2 has acknowledged our FIN and is waiting for the peer's.
         * A peer that abandons the connection (no FIN, no RST) would otherwise
         * pin this detached TCB forever and, under load, exhaust the pool. Cap
         * the wait so the slot is reclaimed (cf. Linux tcp_fin_timeout).
         */
        if (tcb->state == TCP_FIN_WAIT_2) {
            if (kernel_ticks >= tcb->timewait_until) {
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
                if (tcb->rto > TCP_RTO_MAX) tcb->rto = TCP_RTO_MAX;

                serial_printf("TCP: retransmit (RTO %d, try %d)\n",
                              (unsigned)tcb->rto, (int)tcb->rto_backoff);

                if (tcb->send_head != tcb->send_ack) {
                    /*
                     * Sent-but-unacknowledged data (with or without a FIN
                     * queued behind it): collapse the window (RFC 5681) and
                     * resend the oldest unacknowledged data segment. A FIN
                     * sent by close() must not preempt this, or a lost data
                     * segment is never retransmitted and the teardown
                     * stalls; the FIN's own retransmit resumes once the data
                     * is acknowledged (send_ack reaches send_head). snd_nxt
                     * is left untouched so it stays the high-water mark and
                     * ACKs for data in flight still validate.
                     */
                    uint32_t new_ss = tcb->cwnd >> 1;
                    if (new_ss < 2u * TCP_MSS) new_ss = 2u * TCP_MSS;
                    tcb->ssthresh  = new_ss;
                    tcb->cwnd      = TCP_MSS;
                    tcb->rtt_start = 0;   /* Karn: no RTT sample on a retransmit */
                    tcp_retransmit(tcb);
                } else if (tcb->retx_flags) {
                    /* No data in flight; a control segment
                     * (SYN/SYN-ACK/FIN). */
                    tcp_emit_flags(tcb, tcb->retx_flags, tcb->retx_seq);
                    tcb->rto_timer = tcb->rto;
                }
            }
        }

        if (tcb->zwp_timer > 0) {
            tcb->zwp_timer--;
            if (tcb->zwp_timer == 0 && tcb->snd_wnd == 0) {
                tcp_send_zwp(tcb);
                tcb->zwp_timer = tcb->rto << 1;
                if (tcb->zwp_timer > TCP_RTO_MAX) tcb->zwp_timer = TCP_RTO_MAX;
            }
        }
    }
}

/**
 * Whether a live listener holds `port`. Takes `net_lock` itself.
 *
 * For `bind`, which reports the collision where a server expects to hear about
 * it. The invariant is `tcp_listen`'s; this is the courtesy.
 */
int tcp_port_listening(uint16_t port)
{
    uint32_t flags = net_lock();
    int held = tcp_port_has_listener(port);

    net_unlock(flags);

    return held;
}

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
 * @return 0, `-EADDRINUSE` if a listener already holds the port,
 *         `-EDESTADDRREQ` if the socket never bound one, or `-ENOBUFS` if the
 *         connection pool is full.
 */
int tcp_listen(struct socket *sock, uint16_t port)
{
    struct tcb *tcb;
    uint32_t flags;

    /* This stack assigns no port at `listen`, so a socket that never bound one
     * would listen on port 0, which no segment can reach and which the next
     * caller would then collide with. */
    if (port == 0) {
        return -EDESTADDRREQ;
    }

    /* Allocating from the shared pool races with the IRQ-context allocator. */
    flags = net_lock();

    if (tcp_port_has_listener(port)) {
        net_unlock(flags);
        return -EADDRINUSE;
    }

    tcb = tcb_alloc();
    if (!tcb) {
        net_unlock(flags);
        return -ENOBUFS;
    }

    tcb->local_ip   = net_ip_addr;
    tcb->local_port = port;
    tcb->state      = TCP_LISTEN;
    tcb->sock       = sock;
    sock->tcb       = tcb;

    net_unlock(flags);
    return 0;
}

/**
 * Begin an active open: settle the local port, send the SYN, enter SYN_SENT.
 *
 * Does not block; the handshake completes in tcp_rx and wakes the socket.
 *
 * A socket that bound a port keeps it. What is refused is only the exact triple
 * `tcp_find` keys on, a local port already paired with this same peer address
 * and port, since two such TCBs would have one connection's segments delivered
 * to the other.
 *
 * @return 0, `-EADDRINUSE` for that duplicate, or `-ENOBUFS` if the connection
 *         pool is full.
 */
int tcp_connect(struct socket *sock, uint32_t dst_ip, uint16_t dst_port)
{
    struct tcb *tcb;
    uint32_t flags;
    uint16_t local;

    /* Settle the port and send the SYN atomically against the IRQ side. */
    flags = net_lock();

    /* The port is settled before the TCB is allocated, so neither branch has to
     * reason about the caller's own half-built block; the old loop leaned on
     * the fresh TCB still holding port 0. */
    if (sock->local_port != 0) {
        /* A `bind` before `connect` pins the source port, which is the whole
         * point of binding a client socket; discarding it here made that
         * impossible. */
        local = sock->local_port;
        if (tcp_pair_in_use(local, dst_ip, dst_port)) {
            net_unlock(flags);
            return -EADDRINUSE;
        }
    } else {
        /* Skip local ports already spoken for, listeners included: over
         * loopback an ephemeral that collides with the listener being dialed
         * would give the looped SYN src == dst port, match the caller's own
         * TCB, and hang the handshake. The pool bounds the loop at
         * MAX_TCP_CONNS + 1 increments. */
        do {
            local = next_ephemeral++;
            if (next_ephemeral < 49152) next_ephemeral = 49152;
        } while (tcp_local_port_in_use(local));
    }

    tcb = tcb_alloc();
    if (!tcb) {
        net_unlock(flags);
        return -ENOBUFS;
    }

    tcb->local_ip    = net_ip_addr;
    tcb->local_port  = local;
    tcb->remote_ip   = dst_ip;
    tcb->remote_port = dst_port;
    tcb->iss         = tcp_isn(tcb->local_port, dst_port);
    tcb->snd_nxt     = tcb->iss;
    tcb->snd_una     = tcb->iss;
    tcb->sock        = sock;

    sock->tcb         = tcb;
    sock->local_port  = tcb->local_port;
    sock->remote_ip   = dst_ip;
    sock->remote_port = dst_port;

    tcp_send_flags(tcb, TCP_SYN);
    tcb->state = TCP_SYN_SENT;

    net_unlock(flags);
    return 0;
}

/**
 * Detach the socket and begin closing the connection.
 *
 * In ESTABLISHED or CLOSE_WAIT, flushes queued data and sends a FIN after the
 * last byte (a graceful teardown that outlives the socket); otherwise frees
 * the TCB. The socket is freed by the caller right after this returns, so the
 * TCB is unlinked from it here.
 */
void tcp_close(struct socket *sock)
{
    struct tcb *tcb;
    uint32_t flags;

    if (!sock || !sock->tcb) return;
    tcb = sock->tcb;

    /* Reachable from syscall context (close/exit) and from IRQ context (a reset
     * or a refused connection); take the lock either way - it is a no-op when
     * interrupts are already disabled. */
    flags = net_lock();

    /* Detach both ways: the socket is freed right after this returns. */
    sock->tcb = NULL;
    tcb->sock = NULL;

    switch (tcb->state) {
    case TCP_ESTABLISHED:
    case TCP_CLOSE_WAIT:
        /* Flush remaining data, then send the FIN after the last byte. */
        tcb->fin_pending = 1;
        tcp_pump(tcb);
        break;
    default:
        tcb_free(tcb);
        break;
    }

    net_unlock(flags);
}

/**
 * Report whether a stream socket has a peer (ESTABLISHED or any later state).
 */
int tcp_is_connected(struct socket *sock)
{
    struct tcb *tcb;

    if (!sock || sock->type != SOCK_STREAM || sock->listening) {
        return 0;
    }
    /* Snapshot the TCB pointer once (an IRQ-side tcp_close detaches it by
     * NULLing sock->tcb); re-reading sock->tcb after the NULL check could deref
     * NULL. This is the same access discipline as socket_tcp_recv. */
    tcb = sock->tcb;
    if (!tcb) {
        return 0;
    }
    /* A peer exists from ESTABLISHED through every closing state; the handshake
     * states (SYN_SENT/RCVD), LISTEN, and CLOSED have no peer to name. The
     * state enum is ordered so ESTABLISHED and later compare >=
     * TCP_ESTABLISHED. */
    return tcb->state >= TCP_ESTABLISHED;
}

/**
 * Announce a receive window that reopened because the reader drained the ring.
 *
 * The inbound paths recompute `rcv_wnd` whenever a segment arrives, so the peer
 * learns about a window that SHRANK. Nothing told it about a window that grew:
 * the reader advances `recv_head` in syscall context and sends nothing, so a
 * peer that filled the buffer waited for its own persist timer before probing
 * again. That is what held bulk receive to about a kilobyte a second while
 * transmit ran two hundred times faster.
 *
 * Only ESTABLISHED is worth a segment. In the closing states the peer has
 * either stopped sending or is about to, so an update buys nothing and this
 * stays out of the state machine entirely: the only thing sent is a bare ACK
 * carrying the new window.
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
    uint16_t wnd;

    if (!sock) {
        return;
    }

    flags = net_lock();
    tcb = sock->tcb;
    if (tcb && tcb->state == TCP_ESTABLISHED) {
        wnd = rcvwnd_update(tcb->rcv_wnd, tcb->recv_tail - tcb->recv_head,
                            TCP_RING_SIZE, TCP_MSS);
        if (wnd != 0u) {
            tcb->rcv_wnd = wnd;
            tcp_send_flags(tcb, TCP_ACK);
        }
    }
    net_unlock(flags);
}

/*
 * Advance snd_una and send_ack for an in-window ACK in a state where our own
 * FIN may be outstanding. The FIN's sequence is not data in the ring, so
 * send_ack is advanced by the acknowledged data only (capped at the ring's
 * sent bytes); this keeps the retransmit timer from resending
 * already-acknowledged data after the FIN is sent. The caller has already
 * checked snd_una < ack_num <= snd_nxt.
 */
static void tcp_ack_data(struct tcb *tcb, uint32_t ack_num)
{
    uint32_t acked = ack_num - tcb->snd_una;
    uint32_t ring  = tcb->send_head - tcb->send_ack;

    tcb->send_ack += (acked < ring) ? acked : ring;
    tcb->snd_una   = ack_num;
}

/*
 * Accept `len` bytes of in-order received data and report how many to
 * advance rcv_nxt by. A live socket gets the data delivered (and may take
 * less than `len` when its receive ring is nearly full, so the peer
 * retransmits the rest). Once the socket is detached, which `tcp_close` does
 * before the FIN is even sent, so it holds in ESTABLISHED while queued data
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
 * Process an inbound segment, driving the state machine for its connection.
 *
 * Validates length and checksum, finds the matching connection (or the LISTEN
 * TCB), handles RST, then dispatches on TCP state: completing handshakes,
 * delivering in-order data and acknowledging, and stepping through the closing
 * states. Called in IRQ context off the IP receive path.
 *
 * Simultaneous open (RFC 793) is handled in SYN_SENT: both ends sent a SYN and
 * neither has seen an ACK, so this end answers with a SYN-ACK and waits in
 * SYN_RCVD for the peer's. `snd_nxt` is rewound to `iss` first, and that rewind
 * is the whole of it. The first SYN left `snd_nxt` at `iss + 1`, the send emits
 * at `snd_nxt` and takes its retransmit sequence from it, and the peer's
 * answering SYN-ACK acknowledges `iss + 1`; a SYN-ACK sent from `iss + 1` is
 * therefore acknowledged by nothing, and the connection wedges with `snd_una`,
 * `snd_nxt` and the retransmit sequence inconsistent.
 *
 * That arm is why SYN_RCVD dispatches on `passive`. A passive block was made by
 * a listener and no descriptor owns its socket until `accept` hands one over,
 * so its completion goes on the listener's queue and a reset frees both. An
 * active opener's socket belongs to a descriptor already, so its completion
 * wakes that `connect` (there is no listener to queue it on) and a reset is a
 * refused connect, leaving the socket for its descriptor to close.
 */
void tcp_rx(uint32_t src_ip, uint32_t dst_ip,
            const uint8_t *data, uint16_t len)
{
    uint16_t src_port, dst_port;
    uint32_t seq, ack_num;
    uint8_t  data_off, flags;
    uint16_t window, hdr_len;
    struct tcb    *tcb;
    struct tcb    *conn_tcb;
    struct socket *conn_sock;
    struct socket *listen_sock;

    (void)dst_ip;

    if (len < TCP_HDR_SIZE || len > TCP_SEG_MAX) {
        return;
    }

    /* Verify the checksum before trusting any field. */
    if (tcp_checksum(src_ip, net_ip_addr, data, len) != 0) {
        serial_printf("TCP: bad checksum, dropped\n");
        return;
    }

    src_port = read16be(data + 0);
    dst_port = read16be(data + 2);
    seq      = read32be(data + 4);
    ack_num  = read32be(data + 8);
    data_off = (data[12] >> 4) & 0xF;
    flags    = data[13];
    window   = read16be(data + 14);

    hdr_len = (uint16_t)(data_off * 4);
    if (hdr_len < TCP_HDR_SIZE || hdr_len > len) {
        return;
    }

    tcb = tcp_find(dst_port, src_ip, src_port);
    if (!tcb) {
        if (!(flags & TCP_RST)) {
            uint32_t occupied = (uint32_t)(len - hdr_len);

            if (flags & TCP_SYN) occupied++;
            if (flags & TCP_FIN) occupied++;
            tcp_send_rst_unmatched(src_ip, dst_port, src_port,
                                   flags, seq, ack_num, occupied);
        }
        return;
    }

    /*
     * A reset in any connected or closing state tears the connection down.
     * LISTEN and TIME_WAIT ignore it (no connection to break).
     */
    if (flags & TCP_RST) {
        switch (tcb->state) {
        case TCP_LISTEN:
        case TCP_TIME_WAIT:
            return;
        case TCP_SYN_RCVD: {
            if (tcb->passive) {
                struct socket *s = tcb->sock;
                tcb->sock = NULL;
                if (s) { s->tcb = NULL; socket_unref(s); }
                tcb_free(tcb);
                return;
            }
            tcb->so_error   = ECONNREFUSED;
            tcb->state      = TCP_CLOSED;
            tcb->retx_flags = 0;
            tcb->rto_timer  = 0;
            if (tcb->sock) {
                socket_tcp_connected(tcb->sock);
                socket_tcp_fin(tcb->sock);
            } else {
                tcb_free(tcb);
            }
            return;
        }
        default:
            serial_printf("TCP: reset, connection closed\n");
            /* A reset during the active open is a refused connect; later it is
             * a reset connection. Either is surfaced by SO_ERROR and POLLERR.
             */
            tcb->so_error   = (tcb->state == TCP_SYN_SENT) ? ECONNREFUSED : ECONNRESET;
            tcb->state      = TCP_CLOSED;
            tcb->retx_flags = 0;
            tcb->rto_timer  = 0;
            if (tcb->sock) {
                socket_tcp_connected(tcb->sock);
                socket_tcp_fin(tcb->sock);
            } else {
                tcb_free(tcb);
            }
            return;
        }
    }

    switch (tcb->state) {

    case TCP_LISTEN:
        if (!(flags & TCP_SYN)) return;

        conn_tcb = tcb_alloc();
        if (!conn_tcb) {
            return;
        }
        conn_sock = socket_alloc(SOCK_STREAM);
        if (!conn_sock) {
            tcb_free(conn_tcb);
            return;
        }

        conn_tcb->passive      = 1;
        conn_tcb->local_ip     = net_ip_addr;
        conn_tcb->local_port   = tcb->local_port;
        conn_tcb->remote_ip    = src_ip;
        conn_tcb->remote_port  = src_port;
        conn_tcb->irs          = seq;
        conn_tcb->rcv_nxt      = seq + 1;
        conn_tcb->snd_wnd      = window;
        conn_tcb->iss          = tcp_isn(tcb->local_port, src_port);
        conn_tcb->snd_nxt      = conn_tcb->iss;
        conn_tcb->snd_una      = conn_tcb->iss;
        conn_tcb->sock         = conn_sock;

        conn_sock->tcb         = conn_tcb;
        conn_sock->local_port  = conn_tcb->local_port;
        conn_sock->remote_ip   = src_ip;
        conn_sock->remote_port = src_port;

        tcp_send_flags(conn_tcb, TCP_SYN | TCP_ACK);
        conn_tcb->state = TCP_SYN_RCVD;
        break;

    case TCP_SYN_RCVD:
        if (!(flags & TCP_ACK)) return;
        if (ack_num != tcb->snd_nxt) return;

        tcb->snd_una    = ack_num;
        tcb->snd_wnd    = window;
        tcb->state      = TCP_ESTABLISHED;
        tcb->retx_flags = 0;
        tcb->rto_timer  = 0;

        if (tcb->passive) {
            listen_sock = tcp_find_listen_sock(tcb->local_port);
            if (listen_sock) {
                socket_tcp_accept(listen_sock, tcb->sock);
            }
        } else {
            socket_tcp_connected(tcb->sock);
        }
        break;

    case TCP_SYN_SENT:
        if ((flags & TCP_SYN) && !(flags & TCP_ACK)) {
            tcb->irs     = seq;
            tcb->rcv_nxt = seq + 1;
            tcb->snd_wnd = window;
            tcb->snd_nxt = tcb->iss;
            tcp_send_flags(tcb, TCP_SYN | TCP_ACK);
            tcb->state = TCP_SYN_RCVD;
            return;
        }
        if (!((flags & TCP_SYN) && (flags & TCP_ACK))) return;
        if (ack_num != tcb->snd_nxt) return;

        tcb->irs        = seq;
        tcb->rcv_nxt    = seq + 1;
        tcb->snd_una    = ack_num;
        tcb->snd_wnd    = window;
        tcb->state      = TCP_ESTABLISHED;
        tcb->retx_flags = 0;
        tcb->rto_timer  = 0;

        tcp_send_flags(tcb, TCP_ACK);
        socket_tcp_connected(tcb->sock);
        break;

    case TCP_ESTABLISHED:
    {
        uint16_t       payload_len = (uint16_t)(len - hdr_len);
        const uint8_t *payload     = data + hdr_len;
        int            needs_ack   = 0;

        if (flags & TCP_ACK) {
            if ((int32_t)(ack_num - tcb->snd_una) > 0 &&
                (int32_t)(ack_num - tcb->snd_nxt) <= 0) {
                uint32_t acked = ack_num - tcb->snd_una;

                /* RTT sample (skipped after a retransmit, Karn's algorithm). */
                if (tcb->rtt_start != 0) {
                    uint32_t rtt = kernel_ticks - (tcb->rtt_start - 1);
                    uint32_t new_rto, delta;
                    if (rtt == 0) rtt = 1;
                    if (tcb->srtt == 0) {
                        tcb->srtt   = rtt << 3;
                        tcb->rttvar = rtt << 1;
                    } else {
                        delta = ((tcb->srtt >> 3) > rtt)
                                ? (tcb->srtt >> 3) - rtt
                                : rtt - (tcb->srtt >> 3);
                        tcb->rttvar = tcb->rttvar - (tcb->rttvar >> 2) + delta;
                        tcb->srtt   = tcb->srtt   - (tcb->srtt   >> 3) + rtt;
                    }
                    new_rto = (tcb->srtt >> 3) + tcb->rttvar;
                    if (new_rto < TCP_RTO_MIN) new_rto = TCP_RTO_MIN;
                    if (new_rto > TCP_RTO_MAX) new_rto = TCP_RTO_MAX;
                    tcb->rto       = new_rto;
                    tcb->rtt_start = 0;
                }

                /* Congestion control: slow start then congestion avoidance. */
                if (tcb->cwnd < tcb->ssthresh) {
                    tcb->cwnd += TCP_MSS;
                    if (tcb->cwnd > tcb->ssthresh) tcb->cwnd = tcb->ssthresh;
                } else {
                    uint32_t inc = (uint32_t)TCP_MSS * TCP_MSS / tcb->cwnd;
                    if (inc == 0) inc = 1;
                    tcb->cwnd += inc;
                }

                tcb->snd_una     = ack_num;
                tcb->snd_wnd     = window;
                tcb->send_ack   += acked;
                tcb->rto_backoff = 0;

                if (tcb->snd_una == tcb->snd_nxt) {
                    tcb->rto_timer = 0;   /* everything acknowledged */
                    tcb->zwp_timer = 0;
                } else {
                    tcb->rto_timer = tcb->rto;
                }

                tcp_pump(tcb);
                socket_tcp_writable(tcb->sock); /* freed send-ring space */
            } else if (window != tcb->snd_wnd) {
                /* Pure window update. */
                tcb->snd_wnd = window;
                tcp_pump(tcb);
            }
        }

        /* Deliver in-order data; a gap is dropped but still acknowledged so the
         * sender retransmits. Only the bytes that fit the recv ring advance
         * rcv_nxt, so a truncated tail is not acknowledged and is
         * retransmitted. */
        if (payload_len > 0) {
            if (seq == tcb->rcv_nxt) {
                tcb->rcv_nxt += tcp_accept_data(tcb, payload, payload_len);
            }
            needs_ack = 1;
        }

        /* A FIN is honored only in order and only once. seq + payload_len ==
         * rcv_nxt holds only when the whole payload was accepted, so a FIN
         * riding a truncated segment does not signal EOF before its data is
         * delivered. */
        if ((flags & TCP_FIN) && !tcb->fin_received &&
            seq + payload_len == tcb->rcv_nxt) {
            tcb->rcv_nxt++;
            tcb->fin_received = 1;
            /* The ACK handling above may have already drained a close's queued
             * data and let tcp_pump send our own FIN, moving us to FIN_WAIT_1
             * within this same segment; the peer's FIN then completes a
             * simultaneous close into CLOSING, not CLOSE_WAIT, so the later ACK
             * of our FIN is handled in the state that expects it. */
            tcb->state = (tcb->state == TCP_FIN_WAIT_1) ? TCP_CLOSING
                                                        : TCP_CLOSE_WAIT;
            socket_tcp_fin(tcb->sock);
            needs_ack = 1;
        }

        if (needs_ack) {
            tcb->rcv_wnd = (uint16_t)(TCP_RING_SIZE -
                           (tcb->recv_tail - tcb->recv_head));
            tcp_send_flags(tcb, TCP_ACK);
        }
        break;
    }

    case TCP_FIN_WAIT_1:
    {
        uint16_t       payload_len = (uint16_t)(len - hdr_len);
        const uint8_t *payload     = data + hdr_len;
        int            needs_ack   = 0;

        /* Accept only an ACK within the window (snd_una < ack <= snd_nxt); a
         * higher ack_num would push snd_una past the transmit high-water mark.
         * Advance send_ack by the acknowledged DATA only (capped at the ring's
         * sent bytes), so a lost FIN does not leave RTO retransmitting
         * already-acked data. */
        if ((flags & TCP_ACK) &&
            (int32_t)(ack_num - tcb->snd_una) > 0 &&
            (int32_t)(ack_num - tcb->snd_nxt) <= 0) {
            tcp_ack_data(tcb, ack_num);
            if (ack_num == tcb->snd_nxt) {
                tcb->state          = TCP_FIN_WAIT_2;
                tcb->retx_flags     = 0;
                tcb->rto_timer      = 0;
                tcb->timewait_until = kernel_ticks + TCP_FIN_WAIT2_TICKS;
            }
        }

        /* The peer may keep sending after our close. The application is usually
         * gone by now (close detached tcb->sock), so accept and discard its
         * in-order data, advancing rcv_nxt to acknowledge it so the peer stops
         * retransmitting and the FIN below is consumed in order; a
         * still-attached socket gets it delivered. */
        if (payload_len > 0) {
            if (seq == tcb->rcv_nxt) {
                tcb->rcv_nxt += tcp_accept_data(tcb, payload, payload_len);
            }
            needs_ack = 1;
        }

        /* Consume the peer's FIN only in order; a duplicate or early FIN is
         * just re-acknowledged so the peer stops retransmitting it. */
        if (flags & TCP_FIN) {
            if (seq + payload_len == tcb->rcv_nxt) {
                tcb->rcv_nxt++;
                tcb->state = (tcb->state == TCP_FIN_WAIT_2) ? TCP_TIME_WAIT
                                                            : TCP_CLOSING;
                if (tcb->state == TCP_TIME_WAIT) {
                    tcb->timewait_until = kernel_ticks + TCP_2MSL_TICKS;
                }
            }
            needs_ack = 1;
        }

        if (needs_ack) {
            tcb->rcv_wnd = (uint16_t)(TCP_RING_SIZE -
                           (tcb->recv_tail - tcb->recv_head));
            tcp_send_flags(tcb, TCP_ACK);
        }
        break;
    }

    case TCP_FIN_WAIT_2:
    {
        uint16_t       payload_len = (uint16_t)(len - hdr_len);
        const uint8_t *payload     = data + hdr_len;
        int            needs_ack   = 0;

        /* As in FIN_WAIT_1, the application is usually gone, so accept and
         * discard in-order data (advancing rcv_nxt to acknowledge it); a
         * still-attached socket gets it delivered. */
        if (payload_len > 0) {
            if (seq == tcb->rcv_nxt) {
                tcb->rcv_nxt += tcp_accept_data(tcb, payload, payload_len);
            }
            needs_ack = 1;
        }
        if (flags & TCP_FIN) {
            if (seq + payload_len == tcb->rcv_nxt) {
                tcb->rcv_nxt++;
                tcb->state          = TCP_TIME_WAIT;
                tcb->timewait_until = kernel_ticks + TCP_2MSL_TICKS;
            }
            needs_ack = 1;
        }
        if (needs_ack) {
            tcb->rcv_wnd = (uint16_t)(TCP_RING_SIZE -
                           (tcb->recv_tail - tcb->recv_head));
            tcp_send_flags(tcb, TCP_ACK);
        }
        break;
    }

    case TCP_CLOSING:
        if ((flags & TCP_ACK) && ack_num == tcb->snd_nxt) {
            tcb->retx_flags     = 0;
            tcb->rto_timer      = 0;
            tcb->state          = TCP_TIME_WAIT;
            tcb->timewait_until = kernel_ticks + TCP_2MSL_TICKS;
        } else {
            if ((flags & TCP_ACK) &&
                (int32_t)(ack_num - tcb->snd_una) > 0 &&
                (int32_t)(ack_num - tcb->snd_nxt) < 0) {
                tcp_ack_data(tcb, ack_num); /* a data-only ACK frees the ring */
            }
            if (flags & TCP_FIN) {
                tcp_send_flags(tcb, TCP_ACK); /* re-ACK a retransmitted FIN */
            }
        }
        break;

    case TCP_LAST_ACK:
        if ((flags & TCP_ACK) && ack_num == tcb->snd_nxt) {
            tcb_free(tcb);
        } else {
            if ((flags & TCP_ACK) &&
                (int32_t)(ack_num - tcb->snd_una) > 0 &&
                (int32_t)(ack_num - tcb->snd_nxt) < 0) {
                tcp_ack_data(tcb, ack_num); /* a data-only ACK frees the ring */
            }
            if (flags & TCP_FIN) {
                tcp_send_flags(tcb, TCP_ACK); /* re-ACK a retransmitted FIN */
            }
        }
        break;

    case TCP_CLOSE_WAIT:
        /* Acknowledge our sent data and keep draining a pending FIN. */
        if ((flags & TCP_ACK) &&
            (int32_t)(ack_num - tcb->snd_una) > 0 &&
            (int32_t)(ack_num - tcb->snd_nxt) <= 0) {
            tcb->send_ack += ack_num - tcb->snd_una;
            tcb->snd_una   = ack_num;
            tcb->snd_wnd   = window;
            if (tcb->snd_una == tcb->snd_nxt) {
                tcb->rto_timer = 0;
            }
            tcp_pump(tcb);
            socket_tcp_writable(tcb->sock); /* freed send-ring space */
            break;
        } else if ((flags & TCP_ACK) && window != tcb->snd_wnd) {
            tcb->snd_wnd = window; /* a pure window update reopens a closed window */
        }
        tcp_pump(tcb);
        break;

    case TCP_TIME_WAIT:
        if (flags & TCP_FIN) {
            /* The peer retransmits its FIN because our ACK of it was lost;
             * re-ACK and restart the 2MSL quarantine. */
            tcp_send_flags(tcb, TCP_ACK);
            tcb->timewait_until = kernel_ticks + TCP_2MSL_TICKS;
        }
        break;

    default:
        break;
    }
}
