#ifndef _TCP_H_
#define _TCP_H_

#include "../../lib/kint.h"
#include "../socket/address.h"
#include "../../timer.h"

/*
 * tcp: the Transmission Control Protocol. Each connection, and each
 * listener, is a control block (`struct tcb`) from the kernel heap, on one
 * list. The socket layer opens, sends on and closes connections through the
 * calls below, which take `net_lock` themselves; `ip_rx` gives each segment
 * received to `tcp_rx`, and the timer's tick drives `tcp_tick`, both with
 * interrupts off.
 */

/* The flag bits of a segment's byte 13. */
#define TCP_FIN 0x01
#define TCP_SYN 0x02
#define TCP_RST 0x04
#define TCP_PSH 0x08
#define TCP_ACK 0x10
#define TCP_URG 0x20

/* The header without options, and the largest segment either side sends:
 * a card's largest MTU, 1500 bytes, less 40 bytes of IPv4 and TCP headers.
 * A SYN offers its interface's MTU less the headers, at most this, and a
 * segment sent fits the path's MTU. */
#define TCP_HDR_SIZE        20
#define TCP_MSS             1460
#define TCP_INIT_SEGS       4 /* initial congestion window, in segments (RFC 6928) */
#define TCP_INIT_CWND       (TCP_INIT_SEGS * TCP_MSS)
/* The segment size a peer gets when its SYN has no MSS option (RFC 9293),
 * and the least one may ask for: Linux's floor against a peer that asks
 * for tiny segments to make a sender do more work per byte. */
#define TCP_MSS_DEFAULT     536
#define TCP_MSS_MIN         48
/* The window field's largest value, and the send ring's size: 64 KiB, in
 * the control block, a power of two, since the ring's indexes wrap by a
 * mask, and one byte more than the largest unscaled window, so that a
 * link's round trip and not the ring bounds what is in flight. The window
 * offered is the receive ring's free space clamped to what the field can
 * carry at the connection's scale (`tcp_rcv_window`), so that an empty ring
 * of 64 KiB does not offer 0. */
#define TCP_WINDOW_SIZE     65535
#define TCP_RING_SIZE       65536
/* Policy: a connection's receive ring, a heap allocation apart from the
 * control block, is 64 KiB, as the send ring is, or 512 KiB when the
 * machine has more than TCP_BIG_RING_FRAMES frames free as the connection
 * opens, offered through the window scale option (RFC 7323): a window is
 * what a round trip carries, and a round trip of 67 ms moves 7.8 MB/s in
 * 512 KiB and 978 KB/s in 64 KiB. */
#define TCP_RECV_RING_SMALL 65536u
#define TCP_RECV_RING_LARGE 524288u
#define TCP_BIG_RING_FRAMES 32768u
/* How many connections with queued bytes the tick pumps, in rotation, so
 * that a full card does not hold the tick for every blocked connection's
 * wait and no connection waits behind the others for ever. */
#define TCP_TICK_PUMPS      4
/* Policy: a connection keeps data that arrived past a hole in up to eight
 * ranges of sequence numbers, which covers the losses of one window of a
 * path that loses a few segments of it. */
#define TCP_OOO_RANGES      8
/* The SACK blocks an acknowledgement names at most: four, 36 bytes of
 * options, when a segment has no timestamps (RFC 2018). */
#define TCP_SACK_BLOCKS     4

/* The timers are written in milliseconds and kept in ticks of the kernel's
 * clock; TCP_MS rounds up, so that no timer is shorter than asked or 0. */
#define TCP_MS(ms)              (((uint32_t)(ms) * TICK_HZ + 999u) / 1000u)
/* The retransmission timeout: 1 s before a round trip is measured (RFC
 * 6298), then the smoothed round trip plus four deviations, at least 200 ms,
 * Linux's floor, which covers a peer's delayed acknowledgement, and at most
 * 60 s after doubling. */
#define TCP_RTO_INIT            TCP_MS(1000)
#define TCP_RTO_MIN             TCP_MS(200)
#define TCP_RTO_MAX             TCP_MS(60000)
/* Policy: TIME_WAIT lasts 2 s, short for a local network, and a detached
 * block in FIN_WAIT_2, whose peer may go with neither a FIN nor a reset,
 * 10 s (cf. Linux's tcp_fin_timeout). */
#define TCP_2MSL_TICKS          TCP_MS(2000)
#define TCP_FIN_WAIT2_TICKS     TCP_MS(10000)
/* Policy: a detached block that still has data or a FIN to deliver, in
 * ESTABLISHED, CLOSE_WAIT, FIN_WAIT_1, CLOSING or LAST_ACK, is given up when
 * its peer has acknowledged nothing for 30 s, rather than after the minutes
 * TCP_MAX_RETX doublings can take, since no program waits on it (cf. Linux's
 * tcp_orphan_retries). */
#define TCP_ORPHAN_TICKS        TCP_MS(30000)
/* How long queued bytes may wait from the first transmit the driver refused
 * to the last, with no segment sent between and the link up, before the
 * connection is given up: 30 s. While the link is down the wait does not
 * run, so a cable out for a minute ends nothing. */
#define TCP_REFUSE_GIVEUP_TICKS TCP_MS(30000)
/* A handshake is given up after this many retransmissions of its SYN or
 * SYN-ACK. */
#define TCP_SYN_RETRIES         7
/*
 * The give-up of a synchronized connection is Linux's: the time that
 * `tcp_retries2` retransmissions take when the first waits 200 ms and each
 * doubles up to 120 s, about 924.6 s for the default of 15, measured from
 * the first timeout's segment (`tcp_retx_limit`). These are Linux's figures
 * and not this stack's own bounds above, so that a connection to a peer
 * that went away lasts as long as on Linux. TCP_USER_TIMEOUT replaces the
 * figure with the socket's own, in milliseconds, in every state.
 */
#define TCP_RETRIES2_DEFAULT    15
#define TCP_MODEL_RTO_BASE_MS   200u
#define TCP_MODEL_RTO_MAX_MS    120000u

/*
 * Keepalive, as Linux's: a connection with SO_KEEPALIVE set and nothing
 * queued or in flight that has received nothing for TCP_KEEPIDLE seconds is
 * sent a probe, one old byte, and another each TCP_KEEPINTVL seconds while
 * none is answered; when TCP_KEEPCNT probes went unanswered the connection
 * is reset and closed with ETIMEDOUT. A socket's 0 is the default, the
 * value of /proc/sys/net/ipv4/tcp_keepalive_time, _intvl or _probes. The
 * bounds a socket's value must keep to are Linux's.
 */
#define TCP_KEEPIDLE_DEFAULT  7200
#define TCP_KEEPINTVL_DEFAULT 75
#define TCP_KEEPCNT_DEFAULT   9
#define TCP_KEEPIDLE_MAX      32767
#define TCP_KEEPINTVL_MAX     32767
#define TCP_KEEPCNT_MAX       127

/*
 * Path MTU black-hole detection, as Linux's `tcp_mtu_probing` (RFC 4821).
 * Mode 0, the default as on Linux, does neither, since a stall of a radio
 * link times out as a black hole does and would leave a flow on small
 * segments for TCP_PROBE_INTERVAL. Mode 1 starts it when a full-size segment
 * has timed out TCP_RETRIES1 times in a row: the segment size falls to
 * TCP_BASE_MSS, and halves at each further timeout, down to the floor of
 * TCP_MSS_DEFAULT on IPv4 and TCP_MSS_FLOOR6 on IPv6. Once started, a probe
 * of the size halfway to the largest the path may carry is sent from new
 * data; its acknowledgement raises the segment size to it, and its loss
 * lowers the top of the search. A search that has closed to within
 * TCP_MTU_PROBE_THRESH bytes starts again from the top after
 * TCP_PROBE_INTERVAL, as does one TCP_PROBE_INTERVAL after a timeout
 * lowered the size. Mode 2 starts every connection at TCP_BASE_MSS and
 * probes upward.
 */
#define TCP_MTU_PROBING_DEFAULT 0
#define TCP_RETRIES1            3
#define TCP_BASE_MSS            1024u
#define TCP_MSS_FLOOR6          1220u
#define TCP_MTU_PROBE_THRESH    8u
#define TCP_PROBE_INTERVAL      TCP_MS(600000)

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

/* A connection's control block, or a listener's. */
struct netns;

struct tcb {
    struct tcb *next; /* the list of live control blocks */
    int in_use;
    tcp_state_t state;

    /* 1 when a listener made this block for a SYN it received: no descriptor
     * owns its socket until `accept` gives it one. An active open reaches
     * SYN_RCVD too, through a simultaneous open, and there the socket is the
     * caller's already: its completion wakes the `connect` and goes on no
     * listener's queue, and a reset leaves the socket's reference alone,
     * since the descriptor still has it. */
    int passive;
    struct tcb *listener; /* borrowed until a passive handshake completes */

    uint16_t family; /* socket family, including mapped IPv4 on AF_INET6 */
    /* The network namespace of its socket, NULL for the first: it sends in
     * that namespace and takes segments only from it (`ip_ns`). */
    const struct netns *ns;
    net_addr_t local_ip;
    uint16_t local_port;
    net_addr_t remote_ip;
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
    uint32_t rcv_wnd; /* the window last offered, in bytes      */

    /* Data that arrived past a hole, kept in the receive ring past
     * `recv_tail`, where it belongs, and not yet readable: the ranges
     * [ooo_start, ooo_end) of sequence numbers, in order and apart. */
    uint8_t ooo_count;
    uint32_t ooo_start[TCP_OOO_RANGES];
    uint32_t ooo_end[TCP_OOO_RANGES];

    /*
     * The send ring, of TCP_RING_SIZE bytes in the block, and the receive
     * ring, of `recv_size` bytes from the heap, a power of two; a listener
     * has no receive ring. Each head and tail counts bytes and wraps at
     * 2^32, and its index into the ring is the count masked with the ring's
     * size less one, so the bytes in a ring are its tail less its head in
     * unsigned arithmetic. `send_ack` is the count of the oldest byte not
     * yet acknowledged.
     */
    uint8_t send_buf[TCP_RING_SIZE];
    uint32_t send_head, send_tail; /* head = next to send, tail = next to fill */
    uint8_t *recv_buf;
    uint32_t recv_size;
    uint32_t recv_head, recv_tail; /* head = next to read, tail = next to fill */

    int fin_received; /* the peer's FIN was taken: a read sees the end */
    int fin_pending;  /* a close or shutdown asked for a FIN after the data */

    uint32_t so_error; /* pending SO_ERROR (positive errno); 0 = none */
    /* Why the last send since an acknowledgement found no route, ENETUNREACH
     * for a source address the machine no longer has (positive errno), which
     * a give-up reports in place of ETIMEDOUT; 0 = none. */
    uint32_t soft_error;

    /* Retransmission and congestion control (RFC 6298 / RFC 5681). */
    uint32_t rto;        /* current retransmit timeout (ticks)     */
    uint32_t rto_due;    /* tick the retransmit is due, never 0; 0 = off */
    uint32_t srtt;       /* smoothed RTT x 8 (fixed point)         */
    uint32_t rttvar;     /* RTT variance x 4 (fixed point)         */
    uint32_t rtt_start;  /* ticks+1 at RTT sample start; 0 = none  */
    uint8_t rto_backoff; /* consecutive timeouts; retransmit count */
    uint32_t cwnd;       /* congestion window (bytes)              */
    uint32_t ssthresh;   /* slow-start threshold (bytes)           */
    uint32_t send_ack;   /* send ring position of acknowledged data */
    /* The tick TIME_WAIT ends at, the limit of a detached FIN_WAIT_2, or
     * the limit of any other detached block, which each acknowledgement of
     * new data moves TCP_ORPHAN_TICKS ahead. */
    uint32_t timewait_until;
    uint32_t zwp_due;       /* tick the zero-window probe is due; 0 = off */
    uint32_t tx_refused;    /* transmits the driver refused, a count */
    uint32_t refused_since; /* ticks+1 at the first refusal still unresolved; 0 = none */
    /* ticks+1 at the sending of what the first of the timeouts in a row
     * found unacknowledged, which the give-up counts from; 0 = none. */
    uint32_t retx_stamp;

    /* Keepalive. `ka_armed` is set once the tick has seen SO_KEEPALIVE on
     * the socket, and `ka_since` is the tick of the last segment received
     * or of that arming; `ka_probes` counts the probes unanswered since,
     * the last sent at the tick `ka_probe_at`. */
    uint8_t ka_armed;
    uint8_t ka_probes;
    uint32_t ka_since;
    uint32_t ka_probe_at;

    /* Black-hole detection and probing (TCP_MTU_PROBING_DEFAULT). `mss_max`
     * is the largest segment the peer and the path allow, which `snd_mss`
     * returns to; `mss_hi` the top of the search; `mtup_on` whether the
     * search has started. A probe of `probe_size` bytes at `probe_seq` is in
     * flight while `probe_size` is not 0, and the next may go at the tick
     * `probe_after`. */
    uint8_t mtup_on;
    uint16_t mss_max;
    uint16_t mss_hi;
    uint16_t probe_size;
    uint32_t probe_seq;
    uint32_t probe_after;

    /* The round-trip samples timed to the microsecond, for `ss -i`: the
     * timer above runs on ticks, and a round trip on a LAN is under one. */
    uint32_t rtt_start_us; /* timer_now_us() at the sample's start */
    uint32_t srtt_us;      /* smoothed RTT x 8, microseconds */
    uint32_t rttvar_us;    /* RTT variance x 4, microseconds */

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
    uint8_t ack_owed;   /* a bare acknowledgement the driver refused */
    uint32_t retx_seq;  /* sequence number the control occupies   */

    struct socket *sock; /* owning socket (NULL once detached)     */

    /*
     * The owner and the socket's inode number, for /proc/net/tcp, copied
     * from the socket: `sock` is NULL once the descriptor is closed, and a
     * block in a closing state would otherwise report owner 0, root, and
     * socket 0 for what an ordinary account opened.
     */
    uint32_t uid;
    uint32_t inode;
};

/*
 * The /proc/sys/net/ipv4 settings: tcp_keepalive_time and
 * tcp_keepalive_intvl in seconds, tcp_keepalive_probes, tcp_retries2 and
 * tcp_mtu_probing, read by each connection when it needs them, so that a
 * write applies to the connections open as well as to new ones.
 */
extern int tcp_keepalive_time;
extern int tcp_keepalive_intvl;
extern int tcp_keepalive_probes;
extern int tcp_retries2;
extern int tcp_mtu_probing;

/** Empty the list of control blocks, and have the next `tcp_listen` or
 * `tcp_connect` draw a new key for the initial sequence numbers. Called
 * once, at boot, from `init_network`. */
void tcp_init(void);

/**
 * Take a router's report that a segment sent from `local_ip`:`local_port` to
 * `remote_ip`:`remote_port` at sequence `seq` did not fit a link of `mtu`
 * bytes (RFC 1191): the connection's segments shrink to the MTU less 40
 * bytes of headers, but not below TCP_MSS_DEFAULT, and what is
 * unacknowledged is sent again from the oldest byte, as after a timeout. The
 * largest size black-hole probing may return to falls with it, even when the
 * segments are already smaller. A report for no connection or one still in
 * its handshake, or whose sequence is not in flight, changes nothing. Called
 * with interrupts off, from ICMP's receive path.
 */
void tcp_path_mtu(uint32_t local_ip, uint32_t remote_ip, uint16_t local_port, uint16_t remote_port,
                  uint32_t seq, uint16_t mtu);

/**
 * Whether the sequence number `seq` was sent and is unacknowledged on a
 * connection from `local_ip`:`local_port` to `remote_ip`:`remote_port`,
 * established or later: the check an ICMP report quoting a TCP segment must
 * pass before it may change anything, the shared path MTU among it, since
 * any host can forge one (RFC 5927, 4.1). Called with interrupts off.
 */
int tcp_in_flight(net_addr_t local_ip, uint16_t local_port, net_addr_t remote_ip,
                  uint16_t remote_port, uint32_t seq);

/**
 * Drive every connection's timers, once a tick, from the timer interrupt:
 * free a TIME_WAIT or FIN_WAIT_2 block with no socket whose time is up, send
 * again an acknowledgement the driver refused, retransmit when the
 * retransmission timer runs out and give up with ETIMEDOUT at the limit of
 * `tcp_retx_limit`, give up a detached block whose peer acknowledged
 * nothing for TCP_ORPHAN_TICKS, probe a closed window, send keepalive
 * probes and reset a connection that answers none, and send what refused
 * transmits left in the send rings.
 */
void tcp_tick(void);

/** A control block from the heap, zeroed but for the starting values of a
 * connection, with no receive ring, on the list; NULL when the heap has no
 * room. Called with interrupts off, since the receive side walks the same
 * list. */
struct tcb *tcb_alloc(void);

/** Take `tcb` off the list and free it and its receive ring; NULL is
 * ignored. The caller has interrupts off and has detached the block's
 * socket, since this does not clear `sock->tcb`. */
void tcb_free(struct tcb *tcb);

/**
 * Take the segment of `len` bytes at `data`, header included, sent from
 * `src_ip` to `dst_ip`: check it, find its connection or else its listener,
 * and step that block's state machine (RFC 9293). A malformed segment is
 * dropped, and one that matches no block is answered with a reset unless it
 * is a reset. Counts in `net_mib`. Called with interrupts off, from `ip_rx`:
 * in a card's interrupt, the timer's poll of a card, or a delivery from the
 * loopback ring.
 */
void tcp_rx(uint32_t src_ip, uint32_t dst_ip, const uint8_t *data, uint16_t len);

/** Whether a listener has `port` at an address overlapping sock's binding,
 * including a dual-stack wildcard. Takes net_lock itself. */
int tcp_port_listening(const struct socket *sock, uint16_t port);

/**
 * A port for a stream socket bound to port 0, which Linux gives at the bind:
 * the next from 49152 to 65535 that no control block and no stream socket
 * has, or 0 when none is free. Takes `net_lock` itself.
 */
uint16_t tcp_bind_ephemeral(void);

/**
 * Make `sock` a listener on `port`, at the address it bound or any, or on
 * the next ephemeral port when `port` is 0, and set `sock->local_port` to
 * the port. Returns 0, -EADDRINUSE when a listener has the port at an
 * address that meets this one or no ephemeral port is free, or -ENOBUFS with
 * no memory for a control block. Takes `net_lock` itself.
 */
int tcp_listen(struct socket *sock, uint16_t port);

/**
 * Begin an active open of `sock` to `dst_ip`:`dst_port`, from the address it
 * bound, or else the one `ip_source` chooses, and from its port, or else the
 * next ephemeral one: send the SYN and enter SYN_SENT. Does not wait; the
 * handshake completes in `tcp_rx`, which wakes the socket.
 *
 * Returns 0 when the open has started, -EINVAL from an address in 127/8 to
 * one that is not this machine's, -EADDRINUSE when the socket's port already
 * has a connection to that peer, -EADDRNOTAVAIL when no ephemeral port is
 * free, or -ENOBUFS with no memory for a control block or its receive ring.
 * Takes `net_lock` itself.
 */
int tcp_connect(struct socket *sock, net_addr_t dst_ip, uint16_t dst_port);

/**
 * Queue up to `len` bytes of `buf` on the send ring of `sock`'s connection,
 * and send what the windows allow; the bytes stay in the ring until they are
 * acknowledged. Returns the bytes queued, 0 when the ring is full, or -1 for
 * a socket with no connection, a negative `len`, or a connection not in
 * ESTABLISHED or CLOSE_WAIT or whose sending half is shut. Takes `net_lock`
 * itself.
 */
int tcp_send(struct socket *sock, const uint8_t *buf, int len);

/**
 * Detach `sock` from its connection and begin closing it. In ESTABLISHED or
 * CLOSE_WAIT the queued data goes out and a FIN after it; in FIN_WAIT_2 the
 * wait for the peer's FIN is limited to TCP_FIN_WAIT2_TICKS; in FIN_WAIT_1,
 * CLOSING, LAST_ACK or TIME_WAIT, after a shutdown, the block finishes as it
 * is; in any other state it is freed, and a listener's close first frees the
 * blocks still in their handshake on its port, with their sockets. The
 * caller keeps `sock`. Takes `net_lock` itself, so it may be called with
 * interrupts on or off.
 */
void tcp_close(struct socket *sock);

/**
 * Close the sending half of the connection, as shutdown(SHUT_WR) asks: the
 * queued data goes out and a FIN after it, and the socket stays attached, so
 * the peer's data and its FIN still reach the reader, until the socket's
 * close. A connection not in ESTABLISHED or CLOSE_WAIT is left as it is.
 * Takes `net_lock` itself.
 */
void tcp_shutdown_send(struct socket *sock);

/** Whether `sock`, a stream socket that is not listening, has a connection
 * in ESTABLISHED or a later state, the closing states included. */
int tcp_is_connected(struct socket *sock);

/**
 * Announce a receive window that reopened after the reader drained the ring.
 * Sends at most one bare ACK, and only in ESTABLISHED, FIN_WAIT_1 and
 * FIN_WAIT_2, where the peer may still send. Safe to call after every read: it
 * stays quiet until the window has grown enough to be worth a segment. Takes
 * `net_lock` itself.
 */
void tcp_window_update(struct socket *sock);

/** Move the connections and listeners of the network namespace `ns`, which
 * has ended, to `to`, a namespace no process is in, as its sockets go
 * (`net_ns_gone`). The caller holds `net_lock`. */
void tcp_ns_gone(const struct netns *ns, const struct netns *to);

/* One connection, copied out for a reader of /proc/net/tcp. */
struct tcp_row {
    uint16_t family; /* socket family, including mapped IPv4 on AF_INET6 */
    net_addr_t local_ip;
    uint16_t local_port;
    net_addr_t remote_ip;
    uint16_t remote_port;
    int state;
    uint32_t txq;     /* bytes queued to send, sent and unacknowledged included */
    uint32_t rxq;     /* bytes queued to read; for a listener, connections to accept */
    uint32_t backlog; /* for a listener, the most connections it queues */
    uint32_t uid;
    uint32_t inode;
    /* This stack's own view, after Linux's columns: the peer's window, the
     * congestion window, the bytes in flight, the bytes not yet sent, the
     * retransmission and zero-window timers, the ticks left on each, at
     * least 1 while it runs and 0 when it is off, the transmits the driver
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
    /* What `ss -i` and `ss -o` report: the segment size and the window
     * shifts, whether both sides permit selective acknowledgement, the
     * retransmission timeout and the smoothed round trip and its variance
     * in ticks (the last two in eighths and quarters, as the block keeps
     * them), the slow-start threshold, the timeouts in a row, the loss
     * recovery under way, and the ticks left of TIME_WAIT. */
    uint16_t snd_mss;
    uint8_t snd_wscale;
    uint8_t rcv_wscale;
    uint8_t sack_ok;
    uint8_t rto_backoff;
    uint8_t recovering;
    uint32_t rto;
    uint32_t srtt;
    uint32_t rttvar;
    uint32_t srtt_us;
    uint32_t rttvar_us;
    uint32_t ssthresh;
    uint32_t timewait_left;
};

/**
 * Copy the control blocks into rows of an array from `kmalloc`, set
 * `*rows` to it, and return how many, or -1 with no memory.
 *
 * The blocks are counted under `net_lock`, the array allocated outside it,
 * and the blocks copied under it again, at most as many as were counted. A
 * new block goes at the head of the list, so a block opened between the two
 * is copied and as many of the oldest are left out. `*rows` is NULL when
 * there were none or no memory; otherwise the caller frees it, even when 0
 * is returned.
 */
int tcp_snapshot(struct tcp_row **rows);

/* The connections as /proc/net/sockstat and /proc/net/snmp count them, in
 * the terms Linux counts its sockets in. */
struct tcp_counts {
    int inuse;      /* in a state other than CLOSED and TIME_WAIT */
    int orphan;     /* in such a state with no socket left */
    int tw;         /* in TIME_WAIT */
    int alloc;      /* every block not in TIME_WAIT */
    int estab;      /* ESTABLISHED or CLOSE_WAIT, Linux's CurrEstab */
    uint32_t bytes; /* the blocks and their receive rings */
};

/** Count the control blocks into `c`, under `net_lock`. */
void tcp_count(struct tcp_counts *c);

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

/** Deliver IPv6 TCP with its receiving interface's scope; interrupts off. */
void tcp6_rx(int ifindex, const uint8_t *src, const uint8_t *dst, const uint8_t *data,
             uint16_t len);
#endif /* _TCP_H_ */
