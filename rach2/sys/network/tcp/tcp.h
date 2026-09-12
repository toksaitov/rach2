#ifndef _TCP_H_
#define _TCP_H_

#include "../../lib/kint.h"

/*
 * Transmission Control Protocol. Each connection is a TCB drawn from a static
 * pool. The state machine is in tcp.c; the socket layer drives passive and
 * active opens, and ip_rx feeds every incoming segment to tcp_rx.
 */

/* TCP flag bits (segment byte 13). */
#define TCP_FIN  0x01
#define TCP_SYN  0x02
#define TCP_RST  0x04
#define TCP_PSH  0x08
#define TCP_ACK  0x10
#define TCP_URG  0x20

/* Protocol constants. */
#define TCP_HDR_SIZE     20
#define TCP_MSS          1460
#define TCP_INIT_CWND    (4 * TCP_MSS)  /* initial congestion window (RFC 6928) */
/* Policy: a connection's send and receive rings are 64 KiB each, in the
 * control block, a power of two since the rings' indexes wrap by a mask,
 * and the largest an unscaled window can name, so that a link's round trip
 * and not the ring bounds what is in flight. The window offered is the
 * receive ring's free space clamped to the field's 65535, through
 * `tcp_rcv_window`, so that an empty ring never advertises zero. */
#define TCP_WINDOW_SIZE  65535
#define TCP_RING_SIZE    65536
/* How long queued bytes may wait from the first transmit the driver
 * refused to the last, with no segment sent between, before the connection
 * is given up, in ticks: 30 s, within the retransmission give-up. */
#define TCP_REFUSE_GIVEUP_TICKS 3000
/* How many connections with queued bytes the tick pumps, in rotation, so
 * that a full card does not hold the tick for every blocked connection's
 * wait and no connection waits behind the others for ever. */
#define TCP_TICK_PUMPS 4

/* Reliability constants (100 Hz clock). */
#define TCP_RTO_INIT    100     /* initial RTO  = 1 s (RFC 6298) */
#define TCP_RTO_MIN     100     /* minimum RTO  = 1 s   */
#define TCP_RTO_MAX     6000    /* maximum RTO  = 60 s  */
#define TCP_2MSL_TICKS  200     /* TIME_WAIT    = 2 s (short: local network) */
#define TCP_FIN_WAIT2_TICKS 1000 /* FIN_WAIT_2  = 10 s (peer-FIN backstop) */
#define TCP_MAX_RETX    7       /* give up after this many retransmissions */

/* Loss recovery (RFC 5681, RFC 6582). The third duplicate acknowledgement
 * in a row means a segment was lost and the ones after it arrived. After a
 * timeout the unacknowledged data is sent again from the oldest byte as
 * the congestion window grows; after duplicates the hole is sent again at
 * once and each partial acknowledgement names the next. */
#define TCP_DUP_ACKS     3
#define TCP_RECOVER_RTO  1
#define TCP_RECOVER_FAST 2

/* TCP connection states (RFC 793). */
typedef enum {
    TCP_CLOSED = 0,
    TCP_LISTEN,
    TCP_SYN_SENT,
    TCP_SYN_RCVD,
    TCP_ESTABLISHED,
    TCP_FIN_WAIT_1,
    TCP_FIN_WAIT_2,
    TCP_CLOSE_WAIT,
    TCP_CLOSING,
    TCP_LAST_ACK,
    TCP_TIME_WAIT
} tcp_state_t;

struct socket; /* defined in socket/socket.h */

/* Per-connection control block. */
struct tcb {
    struct tcb *next; /* the list of live control blocks */
    int         in_use;
    tcp_state_t state;

    /* Whether this block came from a passive open, meaning a listener made it
     * for an incoming SYN and no descriptor owns its socket until `accept`
     * hands one over. An active open reaches SYN_RCVD too, through the
     * simultaneous-open arm, and there the socket is the caller's already: it
     * must be woken as a completed `connect`, not queued on a listener
     * that does not exist, and never unref'd on a reset, since the descriptor
     * still has it. */
    int         passive;

    uint32_t local_ip;
    uint16_t local_port;
    uint32_t remote_ip;
    uint16_t remote_port;

    /* Send sequence space. */
    uint32_t iss;
    uint32_t snd_una;   /* oldest unacknowledged sequence number  */
    uint32_t snd_nxt;   /* next sequence number to send           */
    uint16_t snd_wnd;   /* remote's advertised receive window     */

    /* Receive sequence space. */
    uint32_t irs;
    uint32_t rcv_nxt;   /* next expected sequence number          */
    uint16_t rcv_wnd;   /* our advertised receive window          */

    /*
     * Send and receive rings. head/tail are monotonic uint32_t; the buffer
     * index is the value masked with (TCP_RING_SIZE - 1), so unsigned
     * subtraction always yields the correct occupancy.
     */
    uint8_t  send_buf[TCP_RING_SIZE];
    uint32_t send_head, send_tail; /* head = next to send, tail = next to fill */
    uint8_t  recv_buf[TCP_RING_SIZE];
    uint32_t recv_head, recv_tail; /* head = next to read, tail = next to fill */

    int fin_received;   /* a FIN was received: signals EOF to recv */
    int fin_pending;    /* close() requested: send FIN once data drains */

    uint32_t so_error;  /* pending SO_ERROR (positive errno); 0 = none */

    /* Retransmission and congestion control (RFC 6298 / RFC 5681). */
    uint32_t rto;          /* current retransmit timeout (ticks)     */
    uint32_t rto_timer;    /* countdown to next retransmit; 0 = off  */
    uint32_t srtt;         /* smoothed RTT x 8 (fixed point)         */
    uint32_t rttvar;       /* RTT variance x 4 (fixed point)         */
    uint32_t rtt_start;    /* ticks+1 at RTT sample start; 0 = none  */
    uint8_t  rto_backoff;  /* consecutive timeouts; retransmit count */
    uint32_t cwnd;         /* congestion window (bytes)              */
    uint32_t ssthresh;     /* slow-start threshold (bytes)           */
    uint32_t send_ack;     /* send ring position of acknowledged data */
    uint32_t timewait_until;
    uint32_t zwp_timer;    /* zero-window probe countdown; 0 = off   */
    uint32_t tx_refused;   /* segments the driver would not take, kept */
    uint32_t refused_since; /* ticks+1 at the first refusal still unresolved; 0 = none */

    /* Loss recovery. `recover` is the end of the data sent when the loss
     * was found, and the recovery lasts until an acknowledgement reaches
     * it; after a timeout `retx_nxt` walks the unacknowledged data again.
     * `rtt_seq` is the end of the segment the round-trip sample was started
     * on: the sample ends at the acknowledgement that covers it. */
    uint8_t  recovering;   /* 0, TCP_RECOVER_RTO or TCP_RECOVER_FAST  */
    uint8_t  dup_acks;     /* duplicate acknowledgements in a row     */
    uint8_t  fast_pending; /* a fast resend the driver refused, owed  */
    uint32_t recover;      /* sequence number that ends the recovery  */
    uint32_t retx_nxt;     /* next sequence number to send again      */
    uint32_t rtt_seq;      /* the sample ends at an ACK covering this */
    uint32_t retransmits;  /* segments sent again, for the report     */

    /* Outstanding control segment to retransmit (SYN/SYN-ACK/FIN). */
    uint8_t  retx_flags;   /* 0 = none, else the control flags       */
    uint8_t  ack_owed;     /* a bare acknowledgement the ring refused  */
    uint32_t retx_seq;     /* sequence number the control occupies   */

    struct socket *sock;   /* owning socket (NULL once detached)     */

    /*
     * The owner and the socket number, copied here rather than read through
     * `sock`. A row stays in this pool through the closing states, and `sock`
     * is cleared the moment the descriptor goes, so a connection in TIME_WAIT
     * would otherwise report owner 0 and socket 0: a row that says root owns
     * what an ordinary account opened.
     */
    uint32_t uid;
    uint32_t inode;
};

/* Allocate the TCB pool; called once from init_network. */
void tcp_init(void);

/** Drive the per-connection timers; called once per kernel tick. */
void tcp_tick(void);

/** Allocate a TCB from the shared pool, or NULL if none can be had. */
struct tcb *tcb_alloc(void);

/* Return a TCB to the pool. */
void tcb_free(struct tcb *tcb);

/** Process an inbound TCP segment; called by ip_rx for IP protocol 6. */
void tcp_rx(uint32_t src_ip, uint32_t dst_ip, const uint8_t *data, uint16_t len);

/** Whether a live listener has `port`; for `bind`'s advisory refusal. */
int tcp_port_listening(uint16_t port);

/**
 * Bind a socket to a port in LISTEN for passive opens.
 *
 * 0, `-EADDRINUSE` if a listener already has the port, `-EDESTADDRREQ` if the
 * socket bound none, or `-ENOBUFS` if the connection pool is full. A bare -1
 * would reach userland as `EPERM` through musl's return convention.
 */
int tcp_listen(struct socket *sock, uint16_t port);

/**
 * Begin an active open to dst_ip:dst_port, from the socket's bound port when it
 * has one and from a fresh ephemeral otherwise.
 *
 * 0, `-EADDRINUSE` when that local port is already paired with this exact peer,
 * or `-ENOBUFS` if the connection pool is full.
 */
int tcp_connect(struct socket *sock, uint32_t dst_ip, uint16_t dst_port);

/** Queue up to len bytes for sending; returns the count accepted, -1 on
 * error. */
int tcp_send(struct socket *sock, const uint8_t *buf, int len);

/** Detach the socket and begin closing the connection. */
void tcp_close(struct socket *sock);

/** Report whether a stream socket has a peer (ESTABLISHED or any later
 * state). */
int tcp_is_connected(struct socket *sock);

/**
 * Announce a receive window that reopened after the reader drained the ring.
 * Sends at most one bare ACK, and only from ESTABLISHED. Safe to call after
 * every read: it stays quiet until the window has grown enough to be worth a
 * segment. Takes `net_lock` itself.
 */
void tcp_window_update(struct socket *sock);

/**
 * Abort every connection that was opened from a different local address.
 *
 * A TCB records the address it was opened from, and after an address change
 * that address is somebody else's. Such a connection cannot be continued: its
 * segments would carry a source this machine no longer has, and its peer would
 * answer to a machine that is no longer here. Each is reset locally, with
 * `ENETRESET` on its socket so a blocked read or write returns, rather than
 * left to time out.
 */
void tcp_local_addr_changed(uint32_t new_ip);

/* One connection, copied out for a reader of /proc/net/tcp. */
struct tcp_row {
    uint32_t local_ip;
    uint16_t local_port;
    uint32_t remote_ip;
    uint16_t remote_port;
    int      state;
    uint32_t txq;   /* bytes queued to send, sent and unacknowledged included */
    uint32_t rxq;   /* bytes queued to read */
    uint32_t uid;
    uint32_t inode;
    /* This stack's own view, after Linux's columns: the peer's window, the
     * congestion window, the bytes in flight, the bytes not yet sent, the
     * retransmission and zero-window timers, the transmits the driver
     * refused and how long ago the standing refusal began, in ticks. */
    uint32_t snd_wnd;
    uint32_t cwnd;
    uint32_t in_flight;
    uint32_t unsent;
    uint32_t rto_timer;
    uint32_t zwp_timer;
    uint32_t tx_refused;
    uint32_t refused_for;
    uint32_t retransmits;
};

/**
 * Every live connection as rows in an array from the heap, which the
 * caller frees; how many there are, or -1 when the heap has no room. Taken
 * whole under the lock, so a report is one moment of the list: blocks are
 * made and freed from receive context, and the identity fields come from
 * the block rather than from `sock`, which is cleared while a connection is
 * still closing.
 */
int tcp_snapshot(struct tcp_row **rows);

/**
 * This stack's state as the number Linux's `/proc/net/tcp` uses.
 *
 * The two enumerations do not agree, and a reader prints the name that goes
 * with the number it is given, so an unmapped value would name the wrong state
 * rather than a missing one. Linux's order is ESTABLISHED 1, SYN_SENT 2,
 * SYN_RECV 3, FIN_WAIT1 4, FIN_WAIT2 5, TIME_WAIT 6, CLOSE 7, CLOSE_WAIT 8,
 * LAST_ACK 9, LISTEN 10, CLOSING 11.
 */
int tcp_state_to_linux(int state);

#endif /* _TCP_H_ */
