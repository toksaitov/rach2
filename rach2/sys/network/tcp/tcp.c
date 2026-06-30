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
#include "../socket/socket.h"
#include "../ip/ip.h"
#include "../core/byteorder.h"
#include "../net.h"
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

    /* Bound by the unacknowledged DATA still in the ring, not snd_nxt - snd_una,
     * which also counts an outstanding FIN's sequence; resending that phantom byte
     * would corrupt the stream when a FIN is queued behind unacked data. */
    data_len = tcb->send_tail - tcb->send_ack;
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

/* Abandon a connection that timed out: reset it and wake any blocked syscall. */
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

                if (tcb->retx_flags && tcb->send_ack == tcb->send_tail) {
                    /* A control segment (SYN/SYN-ACK/FIN) with no data outstanding. */
                    tcp_emit_flags(tcb, tcb->retx_flags, tcb->retx_seq);
                    tcb->rto_timer = tcb->rto;
                } else {
                    /*
                     * Unacked data (with or without a FIN queued behind it):
                     * collapse the window (RFC 5681) and resend the oldest
                     * unacknowledged data segment. A FIN sent by close() must not
                     * preempt this, or a lost data segment is never retransmitted
                     * and the teardown stalls; the FIN's own retransmit resumes once
                     * the data is acknowledged (send_ack catches up to send_tail).
                     * snd_nxt is left untouched so it stays the high-water mark and
                     * ACKs for data already in flight still validate.
                     */
                    uint32_t new_ss = tcb->cwnd >> 1;
                    if (new_ss < 2u * TCP_MSS) new_ss = 2u * TCP_MSS;
                    tcb->ssthresh  = new_ss;
                    tcb->cwnd      = TCP_MSS;
                    tcb->rtt_start = 0;   /* Karn: no RTT sample on a retransmit */
                    tcp_retransmit(tcb);
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
 * Bind a socket to a port in the LISTEN state for passive opens.
 */
int tcp_listen(struct socket *sock, uint16_t port)
{
    struct tcb *tcb;
    uint32_t flags;

    /* Allocating from the shared pool races with the IRQ-context allocator. */
    flags = net_lock();

    tcb = tcb_alloc();
    if (!tcb) {
        net_unlock(flags);
        return -1;
    }

    tcb->local_ip   = NET_IP_ADDR;
    tcb->local_port = port;
    tcb->state      = TCP_LISTEN;
    tcb->sock       = sock;
    sock->tcb       = tcb;

    net_unlock(flags);
    return 0;
}

/**
 * Begin an active open: allocate a TCB, send the SYN, and enter SYN_SENT.
 *
 * Does not block; the handshake completes in tcp_rx and wakes the socket.
 */
int tcp_connect(struct socket *sock, uint32_t dst_ip, uint16_t dst_port)
{
    struct tcb *tcb;
    uint32_t flags;

    /* Allocate from the shared pool and send the SYN atomically. */
    flags = net_lock();

    tcb = tcb_alloc();
    if (!tcb) {
        net_unlock(flags);
        return -1;
    }

    tcb->local_ip    = NET_IP_ADDR;
    tcb->local_port  = next_ephemeral++;
    if (next_ephemeral < 49152) next_ephemeral = 49152;
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
    /* Snapshot the TCB pointer once (an IRQ-side tcp_close detaches it by NULLing
     * sock->tcb); re-reading sock->tcb after the NULL check could deref NULL. This
     * is the same access discipline as socket_tcp_recv. */
    tcb = sock->tcb;
    if (!tcb) {
        return 0;
    }
    /* A peer exists from ESTABLISHED through every closing state; the handshake
     * states (SYN_SENT/RCVD), LISTEN, and CLOSED have no peer to name. The state
     * enum is ordered so ESTABLISHED and later compare >= TCP_ESTABLISHED. */
    return tcb->state >= TCP_ESTABLISHED;
}

/**
 * Process an inbound segment, driving the state machine for its connection.
 *
 * Validates length and checksum, finds the matching connection (or the LISTEN
 * TCB), handles RST, then dispatches on TCP state: completing handshakes,
 * delivering in-order data and acknowledging, and stepping through the closing
 * states. Called in IRQ context off the IP receive path.
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
    if (tcp_checksum(src_ip, NET_IP_ADDR, data, len) != 0) {
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
            /* The connection never reached the application; free both. */
            struct socket *s = tcb->sock;
            tcb->sock = NULL;
            if (s) { s->tcb = NULL; socket_unref(s); }
            tcb_free(tcb);
            return;
        }
        default:
            serial_printf("TCP: reset, connection closed\n");
            /* A reset during the active open is a refused connect; later it is a
             * reset connection. Either is surfaced by SO_ERROR and POLLERR. */
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

        conn_tcb->local_ip     = NET_IP_ADDR;
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

        listen_sock = tcp_find_listen_sock(tcb->local_port);
        if (listen_sock) {
            socket_tcp_accept(listen_sock, tcb->sock);
        }
        break;

    case TCP_SYN_SENT:
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

        /* Deliver in-order data; a gap is dropped but still acknowledged so
         * the sender retransmits. Only the bytes that fit the recv ring advance
         * rcv_nxt, so a truncated tail is not acknowledged and is retransmitted. */
        if (payload_len > 0) {
            if (seq == tcb->rcv_nxt) {
                tcb->rcv_nxt += socket_tcp_deliver(tcb->sock, payload, payload_len);
            }
            needs_ack = 1;
        }

        /* A FIN is honored only in order and only once. seq + payload_len ==
         * rcv_nxt holds only when the whole payload was accepted, so a FIN riding
         * a truncated segment does not signal EOF before its data is delivered. */
        if ((flags & TCP_FIN) && !tcb->fin_received &&
            seq + payload_len == tcb->rcv_nxt) {
            tcb->rcv_nxt++;
            tcb->fin_received = 1;
            tcb->state = TCP_CLOSE_WAIT;
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

        /* Accept only an ACK within the window (snd_una < ack <= snd_nxt); a higher
         * ack_num would push snd_una past the transmit high-water mark. */
        if ((flags & TCP_ACK) &&
            (int32_t)(ack_num - tcb->snd_una) > 0 &&
            (int32_t)(ack_num - tcb->snd_nxt) <= 0) {
            tcb->snd_una = ack_num;
            if (ack_num == tcb->snd_nxt) {
                tcb->state          = TCP_FIN_WAIT_2;
                tcb->retx_flags     = 0;
                tcb->rto_timer      = 0;
                tcb->timewait_until = kernel_ticks + TCP_FIN_WAIT2_TICKS;
            }
        }

        /* The peer may keep sending after our close; deliver it in order and ACK. */
        if (payload_len > 0) {
            if (seq == tcb->rcv_nxt) {
                tcb->rcv_nxt += socket_tcp_deliver(tcb->sock, payload, payload_len);
            }
            needs_ack = 1;
        }

        /* Consume the peer's FIN only in order; a duplicate or early FIN is just
         * re-acknowledged so the peer stops retransmitting it. */
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

        if (payload_len > 0) {
            if (seq == tcb->rcv_nxt) {
                tcb->rcv_nxt += socket_tcp_deliver(tcb->sock, payload, payload_len);
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
        } else if (flags & TCP_FIN) {
            tcp_send_flags(tcb, TCP_ACK); /* re-ACK a retransmitted FIN */
        }
        break;

    case TCP_LAST_ACK:
        if ((flags & TCP_ACK) && ack_num == tcb->snd_nxt) {
            tcb_free(tcb);
        } else if (flags & TCP_FIN) {
            tcp_send_flags(tcb, TCP_ACK); /* re-ACK a retransmitted FIN */
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
            /* The peer retransmits its FIN because our ACK of it was lost; re-ACK
             * and restart the 2MSL quarantine. */
            tcp_send_flags(tcb, TCP_ACK);
            tcb->timewait_until = kernel_ticks + TCP_2MSL_TICKS;
        }
        break;

    default:
        break;
    }
}
