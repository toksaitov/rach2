#ifndef _TCP_H_
#define _TCP_H_

#include "../../lib/kint.h"

/*
 * Transmission Control Protocol. Each connection is a TCB allocated from the
 * kernel heap and kept on a list. The state machine is in tcp.c; the socket
 * layer drives passive and active opens, and ip_rx feeds every incoming
 * segment to tcp_rx.
 */

/* TCP flag bits (segment byte 13). */
#define TCP_FIN 0x01
#define TCP_SYN 0x02
#define TCP_RST 0x04
#define TCP_PSH 0x08
#define TCP_ACK 0x10
#define TCP_URG 0x20

/* Protocol constants. */
#define TCP_HDR_SIZE            20
#define TCP_MSS                 1460
#define TCP_INIT_SEGS           4 /* initial congestion window, in segments (RFC 6928) */
#define TCP_INIT_CWND           (TCP_INIT_SEGS * TCP_MSS)
/* The segment size a peer gets when its SYN has no MSS option (RFC 9293),
 * and the least one may ask for: Linux's floor against a peer that asks
 * for tiny segments to make a sender do more work per byte. */
#define TCP_MSS_DEFAULT         536
#define TCP_MSS_MIN             48
/* Policy: a connection's send and receive rings are 64 KiB each, in the
 * control block, a power of two since the rings' indexes wrap by a mask,
 * and the largest an unscaled window can name, so that a link's round trip
 * and not the ring bounds what is in flight. The window offered is the
 * receive ring's free space clamped to the field's 65535, through
 * `tcp_rcv_window`, so that an empty ring never advertises zero. */
#define TCP_WINDOW_SIZE         65535
#define TCP_RING_SIZE           65536
/* Policy: a connection's receive ring is 64 KiB, the most an unscaled window
 * offers, or 512 KiB when the machine has more than TCP_BIG_RING_FRAMES
 * frames free as the connection opens, offered through the window scale
 * option (RFC 7323): a window is what a round trip carries, and the Lenovo's
 * 67 ms to kernel.org carries 7.8 MB/s in 512 KiB and 978 KB/s in 64. */
#define TCP_RECV_RING_SMALL     65536u
#define TCP_RECV_RING_LARGE     524288u
#define TCP_BIG_RING_FRAMES     32768u
/* How long queued bytes may wait from the first transmit the driver
 * refused to the last, with no segment sent between, before the connection
 * is given up, in ticks: 30 s, within the retransmission give-up. */
#define TCP_REFUSE_GIVEUP_TICKS 3000
/* How many connections with queued bytes the tick pumps, in rotation, so
 * that a full card does not hold the tick for every blocked connection's
 * wait and no connection waits behind the others for ever. */
#define TCP_TICK_PUMPS          4
/* Policy: a connection keeps data that arrived past a hole in up to eight
 * ranges of sequence numbers, which covers the losses of one window of a
 * path that loses a few segments of it. */
#define TCP_OOO_RANGES          8
/* The SACK blocks an acknowledgement names at most: four, 36 bytes of
 * options, when a segment has no timestamps (RFC 2018). */
#define TCP_SACK_BLOCKS         4

/* Reliability constants (100 Hz clock). */
#define TCP_RTO_INIT        100  /* initial RTO  = 1 s (RFC 6298) */
#define TCP_RTO_MIN         100  /* minimum RTO  = 1 s   */
#define TCP_RTO_MAX         6000 /* maximum RTO  = 60 s  */
#define TCP_2MSL_TICKS      200  /* TIME_WAIT    = 2 s (short: local network) */
#define TCP_FIN_WAIT2_TICKS 1000 /* FIN_WAIT_2  = 10 s (peer-FIN backstop) */
#define TCP_MAX_RETX        7    /* give up after this many retransmissions */

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
    int in_use;
    tcp_state_t state;

    /* Whether this block came from a passive open, meaning a listener made it
     * for an incoming SYN and no descriptor owns its socket until `accept`
     * hands one over. An active open reaches SYN_RCVD too, through the
     * simultaneous-open arm, and there the socket is the caller's already: it
     * must be woken as a completed `connect`, not queued on a listener
     * that does not exist, and never unref'd on a reset, since the descriptor
     * still has it. */
    int passive;

    uint32_t local_ip;
    uint16_t local_port;
    uint32_t remote_ip;
    uint16_t remote_port;

    /* Send sequence space. */
    uint32_t iss;
    uint32_t snd_una; /* oldest unacknowledged sequence number  */
    uint32_t snd_nxt; /* next sequence number to send           */
    uint32_t snd_wnd; /* remote's receive window, in bytes      */
    uint16_t snd_mss; /* largest segment the remote takes       */
    /* Window scaling (RFC 7323): the shifts of the remote's window field and
     * of this side's, 0 when either SYN lacked the option, and whether this
     * side's SYN carries it. */
    uint8_t snd_wscale;
    uint8_t rcv_wscale;
    uint8_t ws_offer;
    /* Selective acknowledgement (RFC 2018), on the receiving side only:
     * whether this side's SYN offers it, whether both SYNs had it, and the
     * starts of the last segments kept past a hole, the newest first, whose
     * ranges an acknowledgement names first, in that order. */
    uint8_t sack_offer;
    uint8_t sack_ok;
    uint8_t sack_recent_count;
    uint32_t sack_recent[TCP_SACK_BLOCKS];

    /* Receive sequence space. */
    uint32_t irs;
    uint32_t rcv_nxt; /* next expected sequence number          */
    uint32_t rcv_wnd; /* our advertised receive window, bytes   */

    /* Data that arrived past a hole, kept in the receive ring past
     * `recv_tail`, where it belongs, and not yet readable: the ranges
     * [ooo_start, ooo_end) of sequence numbers, in order and apart. */
    uint8_t ooo_count;
    uint32_t ooo_start[TCP_OOO_RANGES];
    uint32_t ooo_end[TCP_OOO_RANGES];

    /*
     * Send and receive rings. head/tail are monotonic uint32_t; the buffer
     * index is the value masked with the ring's size less one, TCP_RING_SIZE
     * for the send ring and `recv_size` for the receive ring, which is from
     * the heap and a power of two, so unsigned subtraction always yields the
     * correct occupancy. A listener has no receive ring.
     */
    uint8_t send_buf[TCP_RING_SIZE];
    uint32_t send_head, send_tail; /* head = next to send, tail = next to fill */
    uint8_t *recv_buf;
    uint32_t recv_size;
    uint32_t recv_head, recv_tail; /* head = next to read, tail = next to fill */

    int fin_received; /* a FIN was received: signals EOF to recv */
    int fin_pending;  /* close() requested: send FIN once data drains */

    uint32_t so_error; /* pending SO_ERROR (positive errno); 0 = none */

    /* Retransmission and congestion control (RFC 6298 / RFC 5681). */
    uint32_t rto;        /* current retransmit timeout (ticks)     */
    uint32_t rto_timer;  /* countdown to next retransmit; 0 = off  */
    uint32_t srtt;       /* smoothed RTT x 8 (fixed point)         */
    uint32_t rttvar;     /* RTT variance x 4 (fixed point)         */
    uint32_t rtt_start;  /* ticks+1 at RTT sample start; 0 = none  */
    uint8_t rto_backoff; /* consecutive timeouts; retransmit count */
    uint32_t cwnd;       /* congestion window (bytes)              */
    uint32_t ssthresh;   /* slow-start threshold (bytes)           */
    uint32_t send_ack;   /* send ring position of acknowledged data */
    uint32_t timewait_until;
    uint32_t zwp_timer;     /* zero-window probe countdown; 0 = off   */
    uint32_t tx_refused;    /* segments the driver would not take, kept */
    uint32_t refused_since; /* ticks+1 at the first refusal still unresolved; 0 = none */

    /* Loss recovery. `recover` is the end of the data sent when the loss
     * was found, and the recovery lasts until an acknowledgement reaches
     * it; after a timeout `retx_nxt` walks the unacknowledged data again.
     * `rtt_seq` is the end of the segment the round-trip sample was started
     * on: the sample ends at the acknowledgement that covers it. */
    uint8_t recovering;   /* 0, TCP_RECOVER_RTO or TCP_RECOVER_FAST  */
    uint8_t dup_acks;     /* duplicate acknowledgements in a row     */
    uint8_t fast_pending; /* a fast resend the driver refused, owed  */
    uint32_t recover;     /* sequence number that ends the recovery  */
    uint32_t retx_nxt;    /* next sequence number to send again      */
    uint32_t rtt_seq;     /* the sample ends at an ACK covering this */
    uint32_t retransmits; /* segments sent again, for the report     */

    /* Outstanding control segment to retransmit (SYN/SYN-ACK/FIN). */
    uint8_t retx_flags; /* 0 = none, else the control flags       */
    uint8_t ack_owed;   /* a bare acknowledgement the ring refused  */
    uint32_t retx_seq;  /* sequence number the control occupies   */

    struct socket *sock; /* owning socket (NULL once detached)     */

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

/* Empty the TCB list; called once from init_network. */
void tcp_init(void);

/**
 * Take a router's report that a segment sent to `remote_ip` from `local_port`
 * to `remote_port` at sequence `seq` did not fit a link of `mtu` bytes: the
 * connection's segments shrink to fit (RFC 1191) and what is unacknowledged
 * goes again. A report whose sequence is not in flight, or which would make
 * the segments larger or smaller than TCP_MSS_DEFAULT, changes nothing.
 * Called from the receive path with interrupts off.
 */
void tcp_path_mtu(uint32_t remote_ip, uint16_t local_port, uint16_t remote_port, uint32_t seq,
                  uint16_t mtu);

/** Drive the per-connection timers; called once per kernel tick. */
void tcp_tick(void);

/** Allocate a TCB from the heap and put it on the list, or NULL if the heap
 * has no room. */
struct tcb *tcb_alloc(void);

/* Take a TCB off the list and free it; its socket must be detached. */
void tcb_free(struct tcb *tcb);

/** Process an inbound TCP segment; called by ip_rx for IP protocol 6. */
void tcp_rx(uint32_t src_ip, uint32_t dst_ip, const uint8_t *data, uint16_t len);

/** Whether a live listener has `port`; for `bind`'s advisory refusal. */
int tcp_port_listening(uint16_t port);

/**
 * A port for a stream socket bound to port 0, which Linux gives at the bind:
 * the next ephemeral port no control block uses, or 0 when none is free.
 */
uint16_t tcp_bind_ephemeral(void);

/**
 * Bind a socket to a port in LISTEN for passive opens, an ephemeral one when
 * `port` is 0.
 *
 * 0, `-EADDRINUSE` if a listener already has the port or no ephemeral port is
 * free, or `-ENOBUFS` if control-block allocation fails. A bare -1 would reach
 * userland as `EPERM` through musl's return convention.
 */
int tcp_listen(struct socket *sock, uint16_t port);

/**
 * Begin an active open to dst_ip:dst_port, from the socket's bound port when it
 * has one and from a fresh ephemeral otherwise.
 *
 * Return 0 when the open has started, `-EADDRINUSE` for a duplicate connection
 * key, `-EADDRNOTAVAIL` when no ephemeral port is available, or `-ENOBUFS` when
 * control-block allocation fails.
 */
int tcp_connect(struct socket *sock, uint32_t dst_ip, uint16_t dst_port);

/**
 * Queue up to `len` bytes and return the count accepted. Return zero when the
 * send ring is full, or -1 for a missing connection, negative length, state
 * other than ESTABLISHED or CLOSE_WAIT, or a sending half shut down.
 *
 * Save and mask interrupts across the connection checks, ring update, and
 * transmit. Retain unacknowledged bytes for retransmission.
 */
int tcp_send(struct socket *sock, const uint8_t *buf, int len);

/** Detach the socket and begin closing the connection. */
void tcp_close(struct socket *sock);

/**
 * Close the sending half of the connection, as shutdown(SHUT_WR) asks: the
 * queued data goes out and a FIN after it, and the socket stays attached, so
 * the peer's data and its FIN still reach the reader, until the socket's
 * close. A connection not in ESTABLISHED or CLOSE_WAIT is left as it is.
 */
void tcp_shutdown_send(struct socket *sock);

/** Report whether a stream socket has a peer (ESTABLISHED or any later
 * state). */
int tcp_is_connected(struct socket *sock);

/**
 * Announce a receive window that reopened after the reader drained the ring.
 * Sends at most one bare ACK, and only in ESTABLISHED, FIN_WAIT_1 and
 * FIN_WAIT_2, where the peer may still send. Safe to call after every read: it
 * stays quiet until the window has grown enough to be worth a segment. Takes
 * `net_lock` itself.
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
    int state;
    uint32_t txq; /* bytes queued to send, sent and unacknowledged included */
    uint32_t rxq; /* bytes queued to read */
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
 * Copy control blocks into a caller-owned array and return the row count.
 *
 * Count under `net_lock`, allocate outside it, then copy under the lock up to
 * the original count. Newly allocated blocks may be included; growth can
 * exclude older blocks. Return -1 on allocation failure. The caller must free
 * a non-NULL result even when the returned count is zero.
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
