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
#define TCP_WINDOW_SIZE  4096
#define TCP_RING_SIZE    4096   /* send/recv ring size, power of two */
#define MAX_TCP_CONNS    16

/* Reliability constants (100 Hz clock). */
#define TCP_RTO_INIT    100     /* initial RTO  = 1 s (RFC 6298) */
#define TCP_RTO_MIN     100     /* minimum RTO  = 1 s   */
#define TCP_RTO_MAX     6000    /* maximum RTO  = 60 s  */
#define TCP_2MSL_TICKS  200     /* TIME_WAIT    = 2 s (short: local network) */
#define TCP_FIN_WAIT2_TICKS 1000 /* FIN_WAIT_2  = 10 s (peer-FIN backstop) */
#define TCP_MAX_RETX    7       /* give up after this many retransmissions */

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
    int         in_use;
    tcp_state_t state;

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

    /* Outstanding control segment to retransmit (SYN/SYN-ACK/FIN). */
    uint8_t  retx_flags;   /* 0 = none, else the control flags       */
    uint32_t retx_seq;     /* sequence number the control occupies   */

    struct socket *sock;   /* owning socket (NULL once detached)     */
};

/* Allocate the TCB pool; called once from init_network. */
void tcp_init(void);

/* Drive per-TCB timers; called from the timer handler at 100 Hz. */
void tcp_tick(void);

/* Pool management. */
struct tcb *tcb_alloc(void);
void tcb_free(struct tcb *tcb);

/* Process a received TCP segment (called by ip_rx for IP protocol 6). */
void tcp_rx(uint32_t src_ip, uint32_t dst_ip, const uint8_t *data, uint16_t len);

/* Passive open: register sock as a listener on port. 0 on success, -1 on error. */
int tcp_listen(struct socket *sock, uint16_t port);

/* Active open: send a SYN to dst_ip:dst_port. The caller then blocks. */
int tcp_connect(struct socket *sock, uint32_t dst_ip, uint16_t dst_port);

/* Queue buf[0..len) for sending and transmit what the windows allow. */
int tcp_send(struct socket *sock, const uint8_t *buf, int len);

/* Begin a graceful close (FIN handshake); detaches the TCB from the socket. */
void tcp_close(struct socket *sock);

/* 1 if the socket has a peer connection past the handshake (ESTABLISHED through
 * the closing states) -- the condition getpeername/shutdown require. 0 for a
 * listener, a still-handshaking (SYN_SENT/RCVD), or closed/never-connected stream
 * (and for any non-stream socket). */
int tcp_is_connected(struct socket *sock);

#endif /* _TCP_H_ */
