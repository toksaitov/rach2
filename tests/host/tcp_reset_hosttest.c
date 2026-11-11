/*
 * tcp_reset_hosttest.c: the segments a host off the path can forge.
 *
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * host-deps: rach2/sys/network/tcp/rcvwnd.c
 * host-deps: rach2/sys/network/socket/address.c rach2/sys/network/ip6/addr6.c
 * host-deps: tests/host/compat/kheap_host.c
 * host-deps: tests/host/compat/pressure_host.c
 *
 * A reset names a connection by its addresses and ports, which anyone can
 * write, and by a sequence number, which only the peer knows. So a reset is
 * taken only at the next sequence number this side expects; one elsewhere in
 * the receive window is answered with an acknowledgement, which the true
 * peer answers with a reset at that number; one outside the window is
 * dropped (RFC 5961, section 3.2). While the connection is still opening
 * there is no window, and a reset counts only when it acknowledges the SYN
 * (RFC 793). A host that could predict this side's initial sequence number
 * could forge the rest of a handshake, so the number is a clock plus a keyed
 * hash of the two addresses and two ports (RFC 6528). And a handshake that
 * completes after its listener closed has nobody to accept it, so it is
 * answered with a reset and freed rather than left open for ever.
 *
 * `tcp.c` is included, as in `tcp_hosttest.c`, with the layer under it
 * replaced by a recorder of every segment `ip_tx` would have sent and the
 * layer above by one that records every callback. The key comes from a
 * stand-in for `krandom_u32` whose four words are the bytes 00 to 0f, the
 * key of SipHash's own test vectors, and the addresses and ports make the
 * twelve bytes 00 to 0b, so the hash of every connection here is a constant
 * the test states.
 */

extern int printf(const char *fmt, ...);

#include "../../rach2/sys/lib/kerrno.h"
#include "../../rach2/sys/lib/kstring.h"
#include "../../rach2/sys/network/tcp/tcp.h"
#include "../../rach2/sys/network/driver/netdev.h"
#include "../../rach2/sys/network/socket/socket.h"

/* ---- the clock, the key, and the layers above and below ---- */

volatile uint64_t kernel_ticks;

/** Return the microsecond clock TCP times its round trips by, for `ss -i`:
 * TICK_US to a tick. */
uint32_t timer_now_us(void)
{
    return (uint32_t)(kernel_ticks * TICK_US);
}

/* This machine and its peer, as little-endian words of the bytes 00 to 07. */
#define SELF      0x03020100u
#define PEER      0x07060504u
/* The two ports, as little-endian halves of the bytes 08 to 0b. */
#define SELF_PORT 0x0908u
#define PEER_PORT 0x0b0au

/* SipHash-2-4 of the twelve bytes 00 to 0b under the key 00 to 0f, the key
 * and message form of SipHash's test vectors, low 32 bits. */
#define SIP_LOW32 0x860ee5fbu

uint32_t net_ip_addr = SELF;

static unsigned key_draws; /* the words krandom_u32 has given */

/** Return the next key word, in the order `tcp_isn_key` stores them, so that
 * each draw of four makes the key 00 to 0f. */
uint32_t krandom_u32(void)
{
    return 0x03020100u + 0x04040404u * (key_draws++ % 4);
}

#define MAX_SEGS 16

static struct {
    uint32_t seq, ack;
    uint8_t flags;
    uint8_t hlen;       /* the header's length in bytes, its options included */
    uint8_t opt;        /* the first option's kind, or 0 for none */
} segs[MAX_SEGS];       /* the first MAX_SEGS segments ip_tx sent */
static unsigned n_segs; /* the segments sent, MAX_SEGS and past it */
static unsigned cb_connected, cb_fin, cb_accept, unrefs; /* calls of each */

uint32_t read32be(const uint8_t *p);

/** Record the sequence and acknowledgement numbers, the flags, the header
 * length and the first option of a segment sent, and return 0. */
int ip_tx(uint32_t dst, uint8_t proto, const uint8_t *payload, uint16_t len, uint8_t ttl)
{
    (void)dst;
    (void)proto;
    (void)ttl;
    if (n_segs < MAX_SEGS) {
        segs[n_segs].seq = read32be(payload + 4);
        segs[n_segs].ack = read32be(payload + 8);
        segs[n_segs].flags = payload[13];
        segs[n_segs].hlen = (uint8_t)((payload[12] >> 4) * 4);
        segs[n_segs].opt = segs[n_segs].hlen > 20 && len > 20 ? payload[20] : 0;
    }
    n_segs++;
    return 0;
}

/** Count a connection that completed or ended. */
void socket_tcp_connected(struct socket *sock)
{
    (void)sock;
    cb_connected++;
}

/** Count the peer's FIN or the connection's end. */
void socket_tcp_fin(struct socket *sock)
{
    (void)sock;
    cb_fin++;
}

/** Note freed send-ring space: a no-op. */
void socket_tcp_writable(struct socket *sock)
{
    (void)sock;
}

/** Take all `len` bytes delivered in order. */
uint16_t socket_tcp_deliver(struct socket *sock, const uint8_t *data, uint16_t len)
{
    (void)sock;
    (void)data;
    return len;
}

/** Take all `len` bytes that arrived past a hole. */
uint16_t socket_tcp_hold(struct socket *sock, uint32_t offset, const uint8_t *data, uint16_t len)
{
    (void)sock;
    (void)offset;
    (void)data;
    return len;
}

/** Make held bytes readable: a no-op. */
void socket_tcp_release(struct socket *sock, uint32_t n)
{
    (void)sock;
    (void)n;
}

/** Count a connection queued on a listener for accept. */
void socket_tcp_accept(struct socket *listen_sock, struct socket *conn_sock)
{
    (void)listen_sock;
    (void)conn_sock;
    cb_accept++;
}

static int forget_queues; /* the accept queues emptied */

/** Count an accept queue emptied by a listener's stop. */
void socket_tcp_forget_queue(struct socket *listen_sock)
{
    (void)listen_sock;
    forget_queues++;
}

/* The sockets a passive open asks for, from a pool the cases reuse, so that
 * nothing here is left for the leak checker. */
static struct socket pool[2];
static unsigned pooled;

/** No other socket has a port bound here. */
int socket_port_bound(uint16_t port)
{
    (void)port;
    return 0;
}

/** Return the next socket of `pool`, cleared, of type `type` and with one
 * reference, or NULL when the pool is used up. */
struct socket *socket_alloc(int type)
{
    struct socket *s;

    if (pooled >= sizeof(pool) / sizeof(pool[0])) {
        return 0;
    }
    s = &pool[pooled++];
    k_memset(s, 0, sizeof(*s));
    s->type = type;
    s->refcount = 1;
    return s;
}

/** Count a socket released. */
void socket_unref(struct socket *sock)
{
    (void)sock;
    unrefs++;
}

/** Read the big-endian 16-bit field at `p`. */
uint16_t read16be(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

/** Read the big-endian 32-bit field at `p`. */
uint32_t read32be(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/** Store `v` at `p` in network byte order. */
void write16be(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xFF);
}

/** Store `v` at `p` in network byte order. */
void write32be(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

/** Return the Internet checksum of `len` bytes at `buf`: the IP header
 * checksum, which `tcp_checksum` reuses for the pseudo-header. */
uint16_t ip_checksum(const uint8_t *buf, uint16_t len)
{
    uint32_t sum = 0;
    uint16_t i;

    for (i = 0; i + 1 < len; i += 2) {
        sum += (uint32_t)(((uint32_t)buf[i] << 8) | buf[i + 1]);
    }
    if (i < len) {
        sum += (uint32_t)((uint32_t)buf[i] << 8);
    }
    while (sum >> 16) {
        sum = (sum & 0xFFFFu) + (sum >> 16);
    }
    return (uint16_t)~sum;
}

/** Take the network lock: a no-op, since no interrupt runs in this model. */
uint32_t net_lock(void)
{
    return 0;
}

/** Release the network lock: a no-op. */
void net_unlock(uint32_t flags)
{
    (void)flags;
}

/** Discard a kernel log record. */
void klog(int level, const char *subsystem, const char *format, ...)
{
    (void)level;
    (void)subsystem;
    (void)format;
}

/** Report whether `ip` is this machine's: the one interface's address, or an
 * address of 127/8. */
int ip_is_local(uint32_t ip)
{
    return ip != 0 && (ip == net_ip_addr || (ip & 0xFF000000u) == 0x7F000000u);
}

/** Return the source address for `dst`: 127.0.0.1 for 127/8, else the
 * interface's address. */
uint32_t ip_source(uint32_t dst)
{
    return (dst & 0xFF000000u) == 0x7F000000u ? 0x7F000001u : net_ip_addr;
}

/* The namespace of the segment being sent or delivered: always the first,
 * the model's only one. */
const struct netns *ip_ns;

/** Record a segment sent from `src`, as ip_tx does. */
int ip_tx_from(uint32_t src, uint32_t dst, uint8_t proto, const uint8_t *payload, uint16_t len,
               uint8_t ttl)
{
    (void)src;
    return ip_tx(dst, proto, payload, len, ttl);
}

#include "compat/tcp_net.h"

#include "../../rach2/sys/network/tcp/tcp.c"

/* The counters /proc/net/snmp reports, which net_core.c defines. */
struct net_mib net_mib;
struct net_mib6 net_mib6;

/* ---- the cases ---- */

static int fails;

#define CHECK(what, cond)                 \
    do {                                  \
        if (!(cond)) {                    \
            printf("  FAIL: %s\n", what); \
            fails++;                      \
        }                                 \
    } while (0)

#define F_FIN 0x01
#define F_SYN 0x02
#define F_RST 0x04
#define F_ACK 0x10

/** Free every block, forget every segment and callback, and set the clock. */
static void reset_all(void)
{
    while (tcbs) {
        tcb_free(tcbs);
    }
    n_segs = 0;
    cb_connected = cb_fin = cb_accept = unrefs = forget_queues = 0;
    pooled = 0;
    kernel_ticks = 1000;
}

/** One segment from the peer's port to `dport` with `plen` bytes of data,
 * at most 16, offering the window `wnd`, checksummed as it is sent. */
static void rx_win(uint16_t dport, uint32_t seq, uint32_t ack, uint8_t flags, uint16_t plen,
                   uint16_t wnd)
{
    uint8_t seg[36];

    k_memset(seg, 'x', sizeof(seg));
    write16be(seg + 0, PEER_PORT);
    write16be(seg + 2, dport);
    write32be(seg + 4, seq);
    write32be(seg + 8, ack);
    seg[12] = 0x50;
    seg[13] = flags;
    write16be(seg + 14, wnd);
    write16be(seg + 16, 0);
    write16be(seg + 18, 0);
    write16be(seg + 16,
              tcp_checksum(net_addr4(PEER), net_addr4(net_ip_addr), seg, (uint16_t)(20 + plen)));
    tcp_rx(PEER, net_ip_addr, seg, (uint16_t)(20 + plen));
}

/** One segment from the peer's port to `dport` with `plen` bytes of data,
 * at most 16, offering a window of 65535. */
static void rx_data(uint16_t dport, uint32_t seq, uint32_t ack, uint8_t flags, uint16_t plen)
{
    rx_win(dport, seq, ack, flags, plen, 65535);
}

/** One segment from the peer's port to `dport`, with no data. */
static void rx(uint16_t dport, uint32_t seq, uint32_t ack, uint8_t flags)
{
    rx_data(dport, seq, ack, flags, 0);
}

/** Open a connection from SELF_PORT to the peer and take its SYN-ACK. */
static struct tcb *open_conn(struct socket *sock, uint32_t peer_seq)
{
    k_memset(sock, 0, sizeof(*sock));
    sock->type = SOCK_STREAM;
    sock->local_port = SELF_PORT;
    n_segs = 0;
    CHECK("a connection can be opened", tcp_connect(sock, net_addr4(PEER), PEER_PORT) == 0);
    rx(SELF_PORT, peer_seq, segs[0].seq + 1, (uint8_t)(F_SYN | F_ACK));
    n_segs = 0;
    return sock->tcb;
}

/**
 * A reset at the next expected sequence number closes the connection; one
 * elsewhere in the window draws an acknowledgement that carries the numbers
 * the true peer must answer with, and closes nothing, counted in
 * TCPChallengeACK; a second within 500 ms draws none, counted in
 * TCPACKSkippedChallenge, and one 500 ms after the first draws one again;
 * one outside the window, below it or past its end, is dropped without an
 * answer. A reset at the right edge of the rightmost SACK block this side
 * sends closes the connection, as Linux's tcp_validate_incoming takes it.
 */
static void test_established(void)
{
    struct socket sock;
    struct tcb *conn;
    uint32_t nxt, challenges = net_mib.tcp_challenge_ack,
                  skipped = net_mib.tcp_ack_skipped_challenge;

    reset_all();
    conn = open_conn(&sock, 5000);
    CHECK("the connection is established", conn && conn->state == TCP_ESTABLISHED);
    nxt = conn->rcv_nxt;
    CHECK("the receive window is open", conn->rcv_wnd > 1000);

    rx(SELF_PORT, nxt + 1000, 0, F_RST);
    CHECK("a reset inside the window but not at the next sequence number closes nothing",
          conn->state == TCP_ESTABLISHED && conn->so_error == 0 && cb_fin == 0);
    CHECK("and draws one acknowledgement at the numbers the peer must answer",
          n_segs == 1 && segs[0].flags == F_ACK && segs[0].seq == conn->snd_nxt &&
              segs[0].ack == nxt);
    CHECK("counted in TCPChallengeACK", net_mib.tcp_challenge_ack == challenges + 1);

    n_segs = 0;
    kernel_ticks += TCP_INVALID_RATELIMIT - 1;
    rx(SELF_PORT, nxt + 1000, 0, F_RST);
    CHECK("a second 499 ms after it draws none, counted in TCPACKSkippedChallenge",
          n_segs == 0 && net_mib.tcp_ack_skipped_challenge == skipped + 1 &&
              conn->state == TCP_ESTABLISHED);
    kernel_ticks++;
    rx(SELF_PORT, nxt + 1000, 0, F_RST);
    CHECK("and a third 500 ms after the first draws one again",
          n_segs == 1 && segs[0].flags == F_ACK && net_mib.tcp_challenge_ack == challenges + 2);

    n_segs = 0;
    kernel_ticks += TCP_INVALID_RATELIMIT;
    rx(SELF_PORT, nxt - 1, 0, F_RST);
    rx(SELF_PORT, conn->rcv_adv + 1, 0, F_RST);
    CHECK("a reset outside the window is dropped without an answer",
          n_segs == 0 && conn->state == TCP_ESTABLISHED && conn->so_error == 0);

    conn->sack_ok = 1;
    conn->ooo_count = 2;
    conn->ooo_start[0] = nxt + 100;
    conn->ooo_end[0] = nxt + 200;
    conn->ooo_start[1] = nxt + 300;
    conn->ooo_end[1] = nxt + 400;
    rx(SELF_PORT, nxt + 200, 0, F_RST);
    CHECK("a reset at the right edge of a SACK block not the rightmost is challenged",
          n_segs == 1 && segs[0].flags == F_ACK && conn->state == TCP_ESTABLISHED);
    rx(SELF_PORT, nxt + 400, 0, F_RST);
    CHECK("and one at the rightmost block's right edge closes the connection",
          conn->state == TCP_CLOSED && conn->so_error == ECONNRESET && cb_fin == 1);
    conn->ooo_count = 0;
    tcp_close(&sock);

    reset_all();
    conn = open_conn(&sock, 5000);
    nxt = conn->rcv_nxt;
    rx(SELF_PORT, nxt, 0, F_RST);
    CHECK("a reset at the next sequence number closes the connection",
          conn->state == TCP_CLOSED && conn->so_error == ECONNRESET && cb_fin == 1 && n_segs == 0);
    tcp_close(&sock);
    reset_all();
}

/**
 * After the peer's FIN, in CLOSE_WAIT, a reset at the FIN's own number,
 * the next expected less one, closes the connection with EPIPE, as Linux's
 * tcp_reset_check takes it and tcp_reset records it, though it is below
 * the window; in ESTABLISHED one there is dropped. A SYN draws a challenge
 * acknowledgement, counted in TCPSYNChallenge, and closes nothing.
 */
static void test_after_fin(void)
{
    struct socket sock;
    struct tcb *conn;
    uint32_t nxt, syns = net_mib.tcp_syn_challenge;

    reset_all();
    conn = open_conn(&sock, 6000);
    nxt = conn->rcv_nxt;
    rx(SELF_PORT, nxt - 1, 0, F_RST);
    CHECK("in ESTABLISHED a reset at the next expected less one is dropped",
          conn->state == TCP_ESTABLISHED && n_segs == 0);
    rx(SELF_PORT, nxt, conn->snd_nxt, (uint8_t)(F_SYN | F_ACK));
    CHECK("a SYN draws a challenge acknowledgement and closes nothing",
          conn->state == TCP_ESTABLISHED && n_segs == 1 && segs[0].flags == F_ACK &&
              segs[0].ack == nxt && net_mib.tcp_syn_challenge == syns + 1);
    rx(SELF_PORT, nxt, conn->snd_nxt, (uint8_t)(F_FIN | F_ACK));
    CHECK("the peer's FIN leads to CLOSE_WAIT",
          conn->state == TCP_CLOSE_WAIT && conn->rcv_nxt == nxt + 1);
    rx(SELF_PORT, nxt, 0, F_RST);
    CHECK("and there a reset at the FIN's number closes the connection with EPIPE",
          conn->state == TCP_CLOSED && conn->so_error == EPIPE);
    tcp_close(&sock);
    reset_all();
}

/**
 * After the peer's FIN, a segment that starts before the next expected
 * number and whose data runs past the FIN ends the connection, as Linux's
 * tcp_rcv_state_process ends it with tcp_reset, counted in TCPAbortOnData,
 * and is answered with a reset at its acknowledgement number, as
 * tcp_v4_do_rcv sends it: in CLOSE_WAIT with EPIPE, and in LAST_ACK, after
 * the program's close, by freeing the block. The FIN sent again alone is
 * acknowledged and ends nothing.
 */
static void test_data_past_fin(void)
{
    struct socket sock;
    struct tcb *conn;
    uint32_t nxt, snd, aborts = net_mib.tcp_abort_on_data;

    reset_all();
    conn = open_conn(&sock, 6500);
    nxt = conn->rcv_nxt;
    rx(SELF_PORT, nxt, conn->snd_nxt, (uint8_t)(F_FIN | F_ACK));
    n_segs = 0;
    rx(SELF_PORT, nxt, conn->snd_nxt, (uint8_t)(F_FIN | F_ACK));
    CHECK("in CLOSE_WAIT the FIN sent again is acknowledged and ends nothing",
          conn->state == TCP_CLOSE_WAIT && n_segs == 1 && segs[0].flags == F_ACK &&
              segs[0].ack == nxt + 1 && net_mib.tcp_abort_on_data == aborts);
    n_segs = 0;
    snd = conn->snd_nxt;
    rx_data(SELF_PORT, nxt, snd, F_ACK, 4);
    CHECK("data from the FIN's number past it ends the connection with EPIPE and a reset",
          conn->state == TCP_CLOSED && conn->so_error == EPIPE && n_segs == 1 &&
              segs[0].flags == F_RST && segs[0].seq == snd &&
              net_mib.tcp_abort_on_data == aborts + 1);
    tcp_close(&sock);

    reset_all();
    conn = open_conn(&sock, 6600);
    nxt = conn->rcv_nxt;
    rx(SELF_PORT, nxt, conn->snd_nxt, (uint8_t)(F_FIN | F_ACK));
    tcp_close(&sock);
    CHECK("a close in CLOSE_WAIT sends the FIN and leads to LAST_ACK",
          tcbs == conn && conn->state == TCP_LAST_ACK);
    n_segs = 0;
    snd = conn->snd_una;
    rx_data(SELF_PORT, nxt, snd, F_ACK, 4);
    CHECK("there data past the peer's FIN frees the block and draws a reset",
          tcbs == 0 && n_segs == 1 && segs[0].flags == F_RST && segs[0].seq == snd &&
              net_mib.tcp_abort_on_data == aborts + 2);
    reset_all();
}

/**
 * After the program's close, data past the next expected number, which no
 * program will read, ends the block with a reset at the segment's
 * acknowledgement number, counted in TCPAbortOnData, as Linux ends a
 * socket whose close set RCV_SHUTDOWN: in FIN_WAIT_1, here moved to
 * FIN_WAIT_2 by the same segment's acknowledgement of the FIN; in a block
 * already in FIN_WAIT_2; and in ESTABLISHED while the FIN waits behind
 * data the peer's closed window holds, where Linux's close has moved to
 * FIN_WAIT_1 already.
 */
static void test_data_after_close(void)
{
    static uint8_t bytes[8];
    struct socket sock;
    struct tcb *conn;
    uint32_t nxt, snd, aborts = net_mib.tcp_abort_on_data;

    reset_all();
    conn = open_conn(&sock, 6700);
    nxt = conn->rcv_nxt;
    tcp_close(&sock);
    CHECK("a close sends the FIN and leads to FIN_WAIT_1",
          tcbs == conn && conn->state == TCP_FIN_WAIT_1);
    n_segs = 0;
    snd = conn->snd_nxt;
    rx_data(SELF_PORT, nxt, snd, F_ACK, 4);
    CHECK("there new data frees the block and draws a reset at its acknowledgement",
          tcbs == 0 && n_segs == 1 && segs[0].flags == F_RST && segs[0].seq == snd &&
              net_mib.tcp_abort_on_data == aborts + 1);

    reset_all();
    conn = open_conn(&sock, 6710);
    nxt = conn->rcv_nxt;
    tcp_close(&sock);
    rx(SELF_PORT, nxt, conn->snd_nxt, F_ACK);
    CHECK("after a close the acknowledgement of the FIN leads to FIN_WAIT_2",
          tcbs == conn && conn->state == TCP_FIN_WAIT_2);
    n_segs = 0;
    snd = conn->snd_nxt;
    rx_data(SELF_PORT, nxt, snd, F_ACK, 4);
    CHECK("there new data frees the block and draws a reset at its acknowledgement",
          tcbs == 0 && n_segs == 1 && segs[0].flags == F_RST && segs[0].seq == snd &&
              net_mib.tcp_abort_on_data == aborts + 2);

    reset_all();
    conn = open_conn(&sock, 6720);
    nxt = conn->rcv_nxt;
    rx_win(SELF_PORT, nxt, conn->snd_nxt, F_ACK, 0, 0);
    CHECK("the peer's window closes and data waits behind it",
          conn->snd_wnd == 0 && tcp_send(&sock, bytes, 8) == 8 && n_segs == 0);
    tcp_close(&sock);
    CHECK("a close queues the FIN behind the data and stays in ESTABLISHED",
          tcbs == conn && conn->state == TCP_ESTABLISHED && conn->fin_pending == 1);
    n_segs = 0;
    snd = conn->snd_una;
    rx_win(SELF_PORT, nxt, snd, F_ACK, 4, 0);
    CHECK("there new data frees the block and draws a reset at its acknowledgement",
          tcbs == 0 && n_segs == 1 && segs[0].flags == F_RST && segs[0].seq == snd &&
              net_mib.tcp_abort_on_data == aborts + 3);
    reset_all();
}

/**
 * A close with received data the program never read ends the connection
 * with a reset at snd_nxt that acknowledges rcv_nxt and frees the block,
 * counted in TCPAbortOnClose, as Linux's __tcp_close does and RFC 1122,
 * 4.2.2.13, asks: from ESTABLISHED counted in EstabResets too, and from
 * FIN_WAIT_1, after a shutdown of the sending half, past the FIN and not
 * in EstabResets.
 */
static void test_close_unread(void)
{
    struct socket sock;
    struct tcb *conn;
    uint32_t nxt, snd, closes = net_mib.tcp_abort_on_close, resets = net_mib.tcp_estab_resets;

    reset_all();
    conn = open_conn(&sock, 6800);
    nxt = conn->rcv_nxt;
    rx_data(SELF_PORT, nxt, conn->snd_nxt, F_ACK, 4);
    conn->recv_tail = conn->recv_head + 4; /* the four bytes wait unread */
    n_segs = 0;
    snd = conn->snd_nxt;
    tcp_close(&sock);
    CHECK("a close with data unread frees the block and sends a reset at snd_nxt",
          tcbs == 0 && sock.tcb == 0 && n_segs == 1 && segs[0].flags == (F_RST | F_ACK) &&
              segs[0].seq == snd && segs[0].ack == nxt + 4);
    CHECK("counted in TCPAbortOnClose and EstabResets",
          net_mib.tcp_abort_on_close == closes + 1 && net_mib.tcp_estab_resets == resets + 1);

    reset_all();
    conn = open_conn(&sock, 6900);
    nxt = conn->rcv_nxt;
    CHECK("a shutdown of the sending half sends the FIN and keeps the block",
          tcp_shutdown(&sock, 0, 1) == 0 && sock.tcb == conn && conn->state == TCP_FIN_WAIT_1 &&
              sock.shutdown == SOCK_SEND_SHUTDOWN && n_segs == 1 &&
              segs[0].flags == (F_FIN | F_ACK));
    rx_data(SELF_PORT, nxt, conn->snd_una, F_ACK, 4);
    CHECK("the socket still takes the peer's data", conn->rcv_nxt == nxt + 4);
    conn->recv_tail = conn->recv_head + 4;
    n_segs = 0;
    snd = conn->snd_nxt;
    tcp_close(&sock);
    CHECK("and a close with it unread sends a reset past the FIN, counted in TCPAbortOnClose",
          tcbs == 0 && n_segs == 1 && segs[0].flags == (F_RST | F_ACK) && segs[0].seq == snd &&
              segs[0].ack == nxt + 4 && net_mib.tcp_abort_on_close == closes + 2 &&
              net_mib.tcp_estab_resets == resets + 1);
    reset_all();
}

/**
 * shutdown(SHUT_RD) alone, which sets RCV_SHUTDOWN on Linux, sends nothing
 * and leaves ESTABLISHED taking the peer's data, as Linux's step 7 queues
 * it there. With SHUT_WR too the FIN goes and the socket keeps its block;
 * data past rcv_nxt in FIN_WAIT_2 then ends the connection with ECONNRESET
 * and a reset at the segment's acknowledgement number, counted in
 * TCPAbortOnData, as Linux's tcp_rcv_state_process ends it, and the block
 * waits, CLOSED, for the socket's close.
 */
static void test_shut_rd(void)
{
    struct socket sock;
    struct tcb *conn;
    uint32_t nxt, snd, aborts = net_mib.tcp_abort_on_data;

    reset_all();
    conn = open_conn(&sock, 7100);
    nxt = conn->rcv_nxt;
    CHECK("SHUT_RD alone is taken and sends nothing",
          tcp_shutdown(&sock, 1, 0) == 0 && sock.shutdown == SOCK_RCV_SHUTDOWN &&
              conn->state == TCP_ESTABLISHED && n_segs == 0);
    rx_data(SELF_PORT, nxt, conn->snd_nxt, F_ACK, 4);
    CHECK("and data after it in ESTABLISHED is taken and acknowledged",
          conn->state == TCP_ESTABLISHED && conn->rcv_nxt == nxt + 4 && n_segs == 1 &&
              segs[0].flags == F_ACK && segs[0].ack == nxt + 4 &&
              net_mib.tcp_abort_on_data == aborts);
    tcp_close(&sock);

    reset_all();
    conn = open_conn(&sock, 7200);
    nxt = conn->rcv_nxt;
    CHECK("SHUT_RDWR sends the FIN and keeps the socket's block",
          tcp_shutdown(&sock, 1, 1) == 0 && sock.tcb == conn && conn->state == TCP_FIN_WAIT_1 &&
              sock.shutdown == (SOCK_RCV_SHUTDOWN | SOCK_SEND_SHUTDOWN) && n_segs == 1 &&
              segs[0].flags == (F_FIN | F_ACK));
    rx(SELF_PORT, nxt, conn->snd_nxt, F_ACK);
    CHECK("the acknowledgement of the FIN leads to FIN_WAIT_2", conn->state == TCP_FIN_WAIT_2);
    n_segs = 0;
    snd = conn->snd_nxt;
    rx_data(SELF_PORT, nxt, snd, F_ACK, 4);
    CHECK("there new data ends the connection with ECONNRESET and a reset at its acknowledgement",
          tcbs == conn && sock.tcb == conn && conn->state == TCP_CLOSED &&
              conn->so_error == ECONNRESET && n_segs == 1 && segs[0].flags == F_RST &&
              segs[0].seq == snd && net_mib.tcp_abort_on_data == aborts + 1);
    tcp_close(&sock);
    CHECK("and the socket's close frees the block with no reset", tcbs == 0 && n_segs == 1);
    reset_all();
}

/**
 * shutdown(2) in SYN_SENT disconnects the socket, as Linux's inet_shutdown
 * has tcp_disconnect do: the block is freed, nothing is sent, the socket
 * records ECONNRESET, gives back the port the open chose and may connect
 * again, and a bound port stays. A socket with no block takes the flags
 * and is refused with ENOTCONN, as Linux's TCP_CLOSE case, and keeps them
 * when it connects again.
 */
static void test_shutdown_syn_sent(void)
{
    struct socket sock;

    reset_all();
    k_memset(&sock, 0, sizeof(sock));
    sock.type = SOCK_STREAM;
    CHECK("an open from an ephemeral port can be started",
          tcp_connect(&sock, net_addr4(PEER), PEER_PORT) == 0 && sock.local_port != 0);
    n_segs = 0;
    CHECK("a shutdown in SYN_SENT frees the block and sends nothing",
          tcp_shutdown(&sock, 0, 1) == 0 && sock.tcb == 0 && tcbs == 0 && n_segs == 0);
    CHECK("the socket records ECONNRESET and gives back the ephemeral port",
          sock.so_error == ECONNRESET && sock.local_port == 0 && sock.shutdown == 0);
    CHECK("a shutdown of a socket with no block takes the flags and is ENOTCONN",
          tcp_shutdown(&sock, 1, 0) == -ENOTCONN && sock.shutdown == SOCK_RCV_SHUTDOWN &&
              tcp_shutdown(&sock, 0, 1) == -ENOTCONN &&
              sock.shutdown == (SOCK_RCV_SHUTDOWN | SOCK_SEND_SHUTDOWN) && n_segs == 0);
    CHECK("and the socket can connect again, keeping them",
          tcp_connect(&sock, net_addr4(PEER), PEER_PORT) == 0 && sock.tcb != 0 &&
              sock.tcb->state == TCP_SYN_SENT &&
              sock.shutdown == (SOCK_RCV_SHUTDOWN | SOCK_SEND_SHUTDOWN));
    tcp_close(&sock);

    reset_all();
    k_memset(&sock, 0, sizeof(sock));
    sock.type = SOCK_STREAM;
    sock.local_port = SELF_PORT;
    CHECK("an open from a bound port can be started",
          tcp_connect(&sock, net_addr4(PEER), PEER_PORT) == 0);
    CHECK("a shutdown in SYN_SENT keeps the bound port", tcp_shutdown(&sock, 1, 0) == 0 &&
                                                             sock.tcb == 0 && tcbs == 0 &&
                                                             sock.local_port == SELF_PORT);
    reset_all();
}

/**
 * shutdown(2) judges the other states as Linux's inet_shutdown: after a
 * simultaneous open, in SYN_RCVD, it takes the flags and sends nothing, as
 * Linux's tcp_shutdown sends no FIN from SYN_RECV, a send waits for the
 * handshake and then fails, and the handshake's end sends no FIN; a block
 * in TIME_WAIT with its socket attached, and one CLOSED by the peer's
 * reset, are Linux's TCP_CLOSE, so the call takes the flags and fails with
 * ENOTCONN, sending nothing.
 */
static void test_shutdown_states(void)
{
    static uint8_t bytes[4];
    struct socket sock;
    struct tcb *conn;
    uint32_t iss, nxt;

    reset_all();
    k_memset(&sock, 0, sizeof(sock));
    sock.type = SOCK_STREAM;
    sock.local_port = SELF_PORT;
    CHECK("an open can be started", tcp_connect(&sock, net_addr4(PEER), PEER_PORT) == 0);
    conn = sock.tcb;
    iss = segs[0].seq;
    rx(SELF_PORT, 7000, 0, F_SYN);
    CHECK("the peer's SYN leads to SYN_RCVD", conn->state == TCP_SYN_RCVD);
    n_segs = 0;
    CHECK("a shutdown in SYN_RCVD takes the flags and sends nothing",
          tcp_shutdown(&sock, 1, 1) == 0 &&
              sock.shutdown == (SOCK_RCV_SHUTDOWN | SOCK_SEND_SHUTDOWN) &&
              conn->state == TCP_SYN_RCVD && conn->fin_pending == 0 && n_segs == 0);
    CHECK("a send there waits for the handshake",
          tcp_send(&sock, bytes, sizeof(bytes)) == TCP_SEND_CONNECTING);
    rx(SELF_PORT, 7001, iss + 1, F_ACK);
    CHECK("whose end sends no FIN", conn->state == TCP_ESTABLISHED && conn->fin_pending == 0 &&
                                        (n_segs == 0 || !(segs[0].flags & F_FIN)));
    CHECK("and the send then fails", tcp_send(&sock, bytes, sizeof(bytes)) == -1);
    tcp_close(&sock);

    reset_all();
    conn = open_conn(&sock, 7300);
    nxt = conn->rcv_nxt;
    CHECK("SHUT_WR sends the FIN", tcp_shutdown(&sock, 0, 1) == 0 && conn->state == TCP_FIN_WAIT_1);
    rx(SELF_PORT, nxt, conn->snd_nxt, F_ACK);
    rx(SELF_PORT, nxt, conn->snd_nxt, (uint8_t)(F_FIN | F_ACK));
    CHECK("the peer's FIN after the acknowledgement leads to TIME_WAIT with the socket attached",
          conn->state == TCP_TIME_WAIT && sock.tcb == conn);
    n_segs = 0;
    CHECK("a shutdown there takes the flags and fails with ENOTCONN",
          tcp_shutdown(&sock, 1, 0) == -ENOTCONN &&
              sock.shutdown == (SOCK_RCV_SHUTDOWN | SOCK_SEND_SHUTDOWN) && n_segs == 0);
    tcp_close(&sock);

    reset_all();
    conn = open_conn(&sock, 7400);
    rx(SELF_PORT, conn->rcv_nxt, 0, F_RST);
    CHECK("a reset leaves the block CLOSED with the socket attached",
          conn->state == TCP_CLOSED && sock.tcb == conn);
    n_segs = 0;
    CHECK("a shutdown there fails with ENOTCONN and sends nothing",
          tcp_shutdown(&sock, 0, 1) == -ENOTCONN && sock.shutdown == SOCK_SEND_SHUTDOWN &&
              n_segs == 0);
    tcp_close(&sock);
    reset_all();
}

/**
 * shutdown(2) of a listener, as Linux's inet_shutdown: SHUT_WR alone leaves
 * it listening; SHUT_RD stops it, as tcp_disconnect has
 * inet_csk_listen_stop do, freeing its block and its handshakes in
 * progress with their sockets and emptying its accept queue, and the socket
 * may listen again.
 */
static void test_shutdown_listener(void)
{
    struct socket lsock;

    reset_all();
    k_memset(&lsock, 0, sizeof(lsock));
    lsock.type = SOCK_STREAM;
    CHECK("a listener can be opened", tcp_listen(&lsock, SELF_PORT) == 0);
    lsock.listening = 1;
    rx(SELF_PORT, 9100, 0, F_SYN);
    CHECK("a handshake starts on it", tcbs != 0 && tcbs->next != 0);
    CHECK("SHUT_WR leaves a listener listening", tcp_shutdown(&lsock, 0, 1) == 0 &&
                                                     lsock.listening == 1 && lsock.tcb != 0 &&
                                                     lsock.shutdown == 0 && forget_queues == 0);
    CHECK("SHUT_RD stops it", tcp_shutdown(&lsock, 1, 0) == 0 && lsock.listening == 0 &&
                                  lsock.tcb == 0 && lsock.shutdown == 0);
    CHECK("freeing its block and its handshake with the socket, and emptying its queue",
          tcbs == 0 && unrefs == 1 && forget_queues == 1);
    CHECK("and it may listen again", tcp_listen(&lsock, SELF_PORT) == 0 && lsock.tcb != 0);
    tcp_close(&lsock);
    reset_all();
}

/**
 * A connection nobody accepted, on a listener that closes or stops, ends as
 * Linux's tcp_disconnect ends it for inet_child_forget: from ESTABLISHED
 * and from CLOSE_WAIT with an RST and ACK at snd_nxt, counted in
 * EstabResets, whether or not data was read, and freed; one CLOSED by the
 * peer's reset is freed with nothing sent.
 */
static void test_forget_queued(void)
{
    struct socket sock;
    struct tcb *conn;
    uint32_t nxt, snd, resets = net_mib.tcp_estab_resets;

    reset_all();
    conn = open_conn(&sock, 7500);
    nxt = conn->rcv_nxt;
    snd = conn->snd_nxt;
    tcp_forget_queued(&sock);
    CHECK("a queued connection in ESTABLISHED is reset at snd_nxt and freed",
          tcbs == 0 && sock.tcb == 0 && n_segs == 1 && segs[0].flags == (F_RST | F_ACK) &&
              segs[0].seq == snd && segs[0].ack == nxt);
    CHECK("counted in EstabResets", net_mib.tcp_estab_resets == resets + 1);

    reset_all();
    conn = open_conn(&sock, 7600);
    nxt = conn->rcv_nxt;
    rx(SELF_PORT, nxt, conn->snd_nxt, (uint8_t)(F_FIN | F_ACK));
    CHECK("the peer's FIN leads to CLOSE_WAIT", conn->state == TCP_CLOSE_WAIT);
    n_segs = 0;
    snd = conn->snd_nxt;
    tcp_forget_queued(&sock);
    CHECK("there it is reset, not given a FIN, counted in EstabResets",
          tcbs == 0 && n_segs == 1 && segs[0].flags == (F_RST | F_ACK) && segs[0].seq == snd &&
              segs[0].ack == nxt + 1 && net_mib.tcp_estab_resets == resets + 2);

    reset_all();
    conn = open_conn(&sock, 7700);
    rx(SELF_PORT, conn->rcv_nxt, 0, F_RST);
    n_segs = 0;
    tcp_forget_queued(&sock);
    CHECK("one the peer reset is freed with nothing sent", tcbs == 0 && n_segs == 0);
    reset_all();
}

/**
 * While the connection is opening, a reset counts only when it acknowledges
 * the SYN. One without the ACK flag leaves the open as it was, even when its
 * acknowledgement field has the number that acknowledges the SYN, and so
 * does one that acknowledges another number.
 */
static void test_syn_sent(void)
{
    struct socket sock;
    struct tcb *conn;
    uint32_t iss;

    reset_all();
    k_memset(&sock, 0, sizeof(sock));
    sock.type = SOCK_STREAM;
    sock.local_port = SELF_PORT;
    CHECK("an open can be started", tcp_connect(&sock, net_addr4(PEER), PEER_PORT) == 0);
    conn = sock.tcb;
    iss = segs[0].seq;
    n_segs = 0;

    rx(SELF_PORT, 777, iss + 1, F_RST);
    CHECK("a reset with the acknowledgement of the SYN but no ACK flag leaves the open",
          conn->state == TCP_SYN_SENT && conn->so_error == 0 && n_segs == 0);
    rx(SELF_PORT, 777, iss, (uint8_t)(F_RST | F_ACK));
    CHECK("a reset that acknowledges another number leaves the open",
          conn->state == TCP_SYN_SENT && conn->so_error == 0 && n_segs == 0);

    rx(SELF_PORT, 0, iss + 1, (uint8_t)(F_RST | F_ACK));
    CHECK("a reset that acknowledges the SYN refuses the connection",
          conn->state == TCP_CLOSED && conn->so_error == ECONNREFUSED);
    tcp_close(&sock);
    reset_all();
}

/**
 * In SYN_RCVD, as Linux's tcp_check_req and tcp_reqsk_send_challenge_ack
 * judge it: a reset inside the window offered but not at the next expected
 * number draws one challenge acknowledgement, counted in TCPChallengeACK;
 * a second in the same tick, one with data among them, draws none, counted
 * in TCPACKSkippedChallenge; one 500 ms after the first draws one again;
 * and a reset at the next expected number frees the passive block. After
 * an active open met the peer's SYN (simultaneous open), such a reset
 * closes the connection with ECONNRESET, as Linux's tcp_reset records it
 * in any state but SYN_SENT and CLOSE_WAIT.
 */
static void test_syn_rcvd(void)
{
    struct socket lsock, sock;
    struct tcb *conn;
    uint32_t iss, challenges = net_mib.tcp_challenge_ack,
                  skipped = net_mib.tcp_ack_skipped_challenge;

    reset_all();
    k_memset(&lsock, 0, sizeof(lsock));
    lsock.type = SOCK_STREAM;
    CHECK("a listener can be opened", tcp_listen(&lsock, SELF_PORT) == 0);
    rx(SELF_PORT, 9000, 0, F_SYN);
    conn = tcbs;
    iss = segs[0].seq;
    CHECK("a SYN makes a block in SYN_RCVD", n_segs == 1 && conn && conn->state == TCP_SYN_RCVD);
    n_segs = 0;
    rx(SELF_PORT, 9001 + 100, 0, F_RST);
    CHECK("a reset in the window not at the next number draws one challenge",
          conn->state == TCP_SYN_RCVD && n_segs == 1 && segs[0].flags == F_ACK &&
              segs[0].seq == iss + 1 && segs[0].ack == 9001 &&
              net_mib.tcp_challenge_ack == challenges + 1);
    n_segs = 0;
    rx(SELF_PORT, 9001 + 200, 0, F_RST);
    rx_data(SELF_PORT, 9001 + 300, 0, F_RST, 8);
    CHECK("two more in the same tick, one with data, draw none, counted as skipped",
          conn->state == TCP_SYN_RCVD && n_segs == 0 &&
              net_mib.tcp_challenge_ack == challenges + 1 &&
              net_mib.tcp_ack_skipped_challenge == skipped + 2);
    kernel_ticks += TCP_INVALID_RATELIMIT;
    rx_data(SELF_PORT, 9001 + 300, 0, F_RST, 8);
    CHECK("one with data 500 ms after the first draws one again",
          n_segs == 1 && segs[0].flags == F_ACK && net_mib.tcp_challenge_ack == challenges + 2);
    rx(SELF_PORT, 9001, 0, F_RST);
    CHECK("a reset at the next number frees the passive block",
          tcbs != 0 && tcbs->next == 0 && tcbs->state == TCP_LISTEN && unrefs == 1);
    tcp_close(&lsock);

    reset_all();
    k_memset(&sock, 0, sizeof(sock));
    sock.type = SOCK_STREAM;
    sock.local_port = SELF_PORT;
    CHECK("an open can be started", tcp_connect(&sock, net_addr4(PEER), PEER_PORT) == 0);
    conn = sock.tcb;
    rx(SELF_PORT, 7000, 0, F_SYN);
    CHECK("the peer's own SYN leads to SYN_RCVD", conn->state == TCP_SYN_RCVD);
    rx(SELF_PORT, 7001, 0, F_RST);
    CHECK("where a reset at the next number closes the connection with ECONNRESET",
          conn->state == TCP_CLOSED && conn->so_error == ECONNRESET);
    tcp_close(&sock);
    reset_all();
}

/**
 * The segments of a handshake in SYN_RCVD that are not resets, as Linux's
 * tcp_check_req judges them for a passive open: the peer's SYN sent again
 * is answered with the SYN-ACK at once, counted in RetransSegs, and a
 * second in the same tick is not, counted in TCPACKSkippedSynRecv; an ACK
 * of another number is answered with a reset at that number and leaves the
 * block; a segment past the window offered draws an acknowledgement, and
 * one with data draws another in the same tick, since Linux's
 * tcp_oow_rate_limited holds back none with data and no SYN; and a SYN at
 * another number inside it is answered with a reset and frees the block,
 * counted in AttemptFails.
 */
static void test_syn_rcvd_segments(void)
{
    struct socket lsock;
    struct tcb *conn;
    uint32_t iss, retrans = net_mib.tcp_retrans_segs, skipped = net_mib.tcp_ack_skipped_syn_recv,
                  fails = net_mib.tcp_attempt_fails;

    reset_all();
    k_memset(&lsock, 0, sizeof(lsock));
    lsock.type = SOCK_STREAM;
    CHECK("a listener can be opened", tcp_listen(&lsock, SELF_PORT) == 0);
    rx(SELF_PORT, 9000, 0, F_SYN);
    conn = tcbs;
    iss = segs[0].seq;
    n_segs = 0;
    rx(SELF_PORT, 9000, 0, F_SYN);
    CHECK("the peer's SYN sent again is answered with the SYN-ACK at once",
          conn->state == TCP_SYN_RCVD && n_segs == 1 && segs[0].flags == (F_SYN | F_ACK) &&
              segs[0].seq == iss && segs[0].ack == 9001 && net_mib.tcp_retrans_segs == retrans + 1);
    rx(SELF_PORT, 9000, 0, F_SYN);
    CHECK("and a second in the same tick is not, counted in TCPACKSkippedSynRecv",
          n_segs == 1 && net_mib.tcp_ack_skipped_syn_recv == skipped + 1);
    n_segs = 0;
    rx(SELF_PORT, 9001, iss + 5, F_ACK);
    CHECK("an ACK of another number is answered with a reset there and leaves the block",
          conn->state == TCP_SYN_RCVD && n_segs == 1 && segs[0].flags == F_RST &&
              segs[0].seq == iss + 5);
    kernel_ticks += TCP_INVALID_RATELIMIT;
    n_segs = 0;
    rx(SELF_PORT, 9001 + 200000, iss + 1, F_ACK);
    CHECK("a segment past the window offered draws an acknowledgement and ends nothing",
          conn->state == TCP_SYN_RCVD && n_segs == 1 && segs[0].flags == F_ACK &&
              segs[0].seq == iss + 1 && segs[0].ack == 9001);
    n_segs = 0;
    rx_data(SELF_PORT, 9001 + 200000, iss + 1, F_ACK, 4);
    CHECK("and one with data in the same tick draws another, held back by no limit",
          conn->state == TCP_SYN_RCVD && n_segs == 1 && segs[0].flags == F_ACK &&
              net_mib.tcp_ack_skipped_syn_recv == skipped + 1);
    n_segs = 0;
    rx(SELF_PORT, 9001 + 100, 0, F_SYN);
    CHECK("a SYN at another number in the window is answered with a reset and frees the block",
          n_segs == 1 && (segs[0].flags & F_RST) && tcbs != 0 && tcbs->next == 0 &&
              tcbs->state == TCP_LISTEN && unrefs == 1 && net_mib.tcp_attempt_fails == fails + 1);

    tcp_close(&lsock);
    reset_all();
}

/**
 * The ACK that ends a passive open's handshake brings its data and FIN
 * with it, which are taken and acknowledged, as Linux's
 * tcp_rcv_state_process takes them once the child is made.
 */
static void test_syn_rcvd_ack_data(void)
{
    struct socket lsock;
    struct tcb *conn;
    uint32_t iss;

    reset_all();
    k_memset(&lsock, 0, sizeof(lsock));
    lsock.type = SOCK_STREAM;
    CHECK("a listener can be opened", tcp_listen(&lsock, SELF_PORT) == 0);
    rx(SELF_PORT, 9500, 0, F_SYN);
    conn = tcbs;
    iss = segs[0].seq;
    n_segs = 0;
    rx_data(SELF_PORT, 9501, iss + 1, (uint8_t)(F_ACK | F_FIN), 4);
    CHECK("the ACK that ends the handshake brings its data and FIN, taken and acknowledged",
          cb_accept == 1 && conn->state == TCP_CLOSE_WAIT && conn->rcv_nxt == 9506 && n_segs == 1 &&
              segs[0].flags == F_ACK && segs[0].ack == 9506);
    tcp_close(&lsock);
    reset_all();
}

/**
 * After a simultaneous open, as Linux's tcp_rcv_state_process judges
 * SYN_RCVD: an ACK of another number draws a reset at that number; a SYN
 * outside the receive window draws a challenge acknowledgement, counted in
 * TCPSYNChallenge, as tcp_validate_incoming answers it; and the peer's
 * SYN-ACK ends the handshake. An ACK of the SYN's own number, snd_una,
 * ends it too, with no reset, as tcp_ack takes it, and leaves the SYN
 * unacknowledged, holding the send ring's slot until an ACK of it takes
 * the slot and its retransmission.
 */
static void test_syn_rcvd_simultaneous(void)
{
    struct socket sock;
    struct tcb *conn;
    uint32_t iss, syns = net_mib.tcp_syn_challenge;

    reset_all();
    k_memset(&sock, 0, sizeof(sock));
    sock.type = SOCK_STREAM;
    sock.local_port = SELF_PORT;
    CHECK("an open can be started", tcp_connect(&sock, net_addr4(PEER), PEER_PORT) == 0);
    conn = sock.tcb;
    iss = segs[0].seq;
    rx(SELF_PORT, 7000, 0, F_SYN);
    n_segs = 0;
    rx(SELF_PORT, 7001, iss + 2, F_ACK);
    CHECK("after a simultaneous open an ACK of another number draws a reset there",
          conn->state == TCP_SYN_RCVD && n_segs == 1 && segs[0].flags == F_RST &&
              segs[0].seq == iss + 2);
    n_segs = 0;
    rx(SELF_PORT, 7001 + 1000000, iss + 1, (uint8_t)(F_SYN | F_ACK));
    CHECK("a SYN outside the window draws a challenge acknowledgement",
          conn->state == TCP_SYN_RCVD && n_segs == 1 && segs[0].flags == F_ACK &&
              segs[0].ack == 7001 && net_mib.tcp_syn_challenge == syns + 1);
    rx(SELF_PORT, 7000, iss + 1, (uint8_t)(F_SYN | F_ACK));
    CHECK("and the peer's SYN-ACK ends the handshake",
          conn->state == TCP_ESTABLISHED && cb_connected == 1);
    tcp_close(&sock);

    reset_all();
    k_memset(&sock, 0, sizeof(sock));
    sock.type = SOCK_STREAM;
    sock.local_port = SELF_PORT;
    CHECK("another open can be started", tcp_connect(&sock, net_addr4(PEER), PEER_PORT) == 0);
    conn = sock.tcb;
    iss = segs[0].seq;
    rx(SELF_PORT, 7000, 0, F_SYN);
    n_segs = 0;
    rx(SELF_PORT, 7001, iss, F_ACK);
    CHECK("an ACK of the SYN's own number ends the handshake with no reset",
          conn->state == TCP_ESTABLISHED && cb_connected == 1 && n_segs == 0 &&
              conn->snd_una == iss && conn->syn_unacked == 1 &&
              conn->send_head - conn->send_ack == 1 && conn->retx_flags == (F_SYN | F_ACK));
    rx(SELF_PORT, 7001, iss + 1, F_ACK);
    CHECK("and an ACK of the SYN takes its slot and its retransmission",
          conn->snd_una == iss + 1 && conn->syn_unacked == 0 && conn->send_head == conn->send_ack &&
              conn->retx_flags == 0 && conn->rto_due == 0);
    tcp_close(&sock);
    reset_all();
}

/**
 * After a simultaneous open ended by an ACK of the SYN's own number, the
 * retransmission timer sends the SYN-ACK at iss, with the options of a
 * SYN, while the SYN is unacknowledged, as Linux's tcp_retransmit_skb
 * sends the SYN at the head of its queue.
 */
static void test_syn_ack_resent(void)
{
    struct socket sock;
    struct tcb *conn;
    uint32_t iss, calls;

    reset_all();
    k_memset(&sock, 0, sizeof(sock));
    sock.type = SOCK_STREAM;
    sock.local_port = SELF_PORT;
    CHECK("an open can be started", tcp_connect(&sock, net_addr4(PEER), PEER_PORT) == 0);
    conn = sock.tcb;
    iss = segs[0].seq;
    rx(SELF_PORT, 7000, 0, F_SYN);
    rx(SELF_PORT, 7001, iss, F_ACK);
    CHECK("an ACK of the SYN's own number leaves the SYN unacknowledged",
          conn->state == TCP_ESTABLISHED && conn->syn_unacked == 1 && conn->rto_due != 0);
    n_segs = 0;
    for (calls = 0; n_segs == 0 && calls < 2u * TCP_RTO_MAX; calls++) {
        kernel_ticks++;
        tcp_tick();
    }
    CHECK("the retransmission timer sends the SYN-ACK at iss with the options of a SYN",
          n_segs == 1 && segs[0].flags == (F_SYN | F_ACK) && segs[0].seq == iss &&
              segs[0].ack == 7001 && segs[0].opt == TCP_OPT_MSS &&
              segs[0].hlen == 24 + (conn->ws_offer ? 4 : 0) + (conn->sack_offer ? 4 : 0));
    tcp_close(&sock);
    reset_all();
}

/**
 * A handshake completed while its listener is open goes on the listener's
 * queue. Closing the listener frees the handshakes still in progress on its
 * port with their sockets, so one completed after the close is answered with
 * a reset at the number the peer acknowledged, and a listener opened on the
 * port since does not receive it.
 */
static void test_listener_gone(void)
{
    struct socket lsock;
    struct socket later;
    uint32_t iss;
    int closed;

    for (closed = 0; closed <= 2; closed++) {
        reset_all();
        k_memset(&lsock, 0, sizeof(lsock));
        lsock.type = SOCK_STREAM;
        CHECK("a listener can be opened", tcp_listen(&lsock, SELF_PORT) == 0);
        rx(SELF_PORT, 9000, 0, F_SYN);
        CHECK("a SYN to the listener is answered with a SYN-ACK",
              n_segs == 1 && segs[0].flags == (F_SYN | F_ACK) && segs[0].ack == 9001);
        iss = segs[0].seq;
        if (closed) {
            tcp_close(&lsock);
            CHECK("closing the listener frees the handshake in progress and its socket",
                  tcbs == 0 && unrefs == 1);
        }
        if (closed == 2) {
            k_memset(&later, 0, sizeof(later));
            later.type = SOCK_STREAM;
            CHECK("a listener can be opened on the port again", tcp_listen(&later, SELF_PORT) == 0);
        }
        n_segs = 0;
        rx(SELF_PORT, 9001, iss + 1, F_ACK);
        if (!closed) {
            CHECK("a handshake completed beside its listener is queued for accept",
                  cb_accept == 1 && n_segs == 0);
            tcp_close(&lsock);
            continue;
        }
        if (closed == 1) {
            CHECK("a handshake completed after its listener closed is answered with a reset",
                  n_segs == 1 && (segs[0].flags & F_RST) && segs[0].seq == iss + 1);
            CHECK("and nobody accepts it", tcbs == 0 && cb_accept == 0);
            continue;
        }
        CHECK("a listener opened after the close does not receive the old handshake",
              cb_accept == 0 && tcbs != 0 && tcbs->next == 0 && tcbs->state == TCP_LISTEN);
        tcp_close(&later);
    }
    reset_all();
}

/**
 * The key is drawn at the first open after `tcp_init` and not again until the
 * next `tcp_init`. An active and a passive open on the same addresses and
 * ports start at the clock, 250000 four-microsecond units a second, plus the
 * keyed hash of those twelve bytes.
 */
static void test_isn(void)
{
    struct socket sock, lsock, other;
    uint32_t want;

    reset_all();
    tcp_init();
    key_draws = 0;
    k_memset(&sock, 0, sizeof(sock));
    sock.type = SOCK_STREAM;
    sock.local_port = SELF_PORT;
    kernel_ticks = 123456;
    want = 123456u * (250000u / TICK_HZ) + SIP_LOW32;
    CHECK("an open can be started", tcp_connect(&sock, net_addr4(PEER), PEER_PORT) == 0);
    CHECK("the first open draws the four words of the key", key_draws == 4);
    CHECK("an active open starts at the clock plus the keyed hash of its addresses and ports",
          n_segs == 1 && segs[0].seq == want && sock.tcb && sock.tcb->iss == want);

    k_memset(&other, 0, sizeof(other));
    other.type = SOCK_STREAM;
    CHECK("a second open can be started", tcp_connect(&other, net_addr4(PEER), 80) == 0);
    k_memset(&lsock, 0, sizeof(lsock));
    lsock.type = SOCK_STREAM;
    CHECK("a listener can be opened", tcp_listen(&lsock, 81) == 0);
    CHECK("later opens and listeners draw no key", key_draws == 4);
    tcp_close(&sock);
    tcp_close(&other);
    tcp_close(&lsock);

    k_memset(&lsock, 0, sizeof(lsock));
    lsock.type = SOCK_STREAM;
    CHECK("the listener can be opened", tcp_listen(&lsock, SELF_PORT) == 0);
    kernel_ticks = 7;
    n_segs = 0;
    rx(SELF_PORT, 4242, 0, F_SYN);
    CHECK("a passive open starts at the clock plus the same keyed hash",
          n_segs == 1 && segs[0].seq == 7u * (250000u / TICK_HZ) + SIP_LOW32);
    tcp_close(&lsock);

    reset_all();
    tcp_init();
    k_memset(&sock, 0, sizeof(sock));
    sock.type = SOCK_STREAM;
    CHECK("an open after tcp_init can be started",
          tcp_connect(&sock, net_addr4(PEER), PEER_PORT) == 0);
    CHECK("tcp_init makes the next open draw a new key", key_draws == 8);
    tcp_close(&sock);
    reset_all();
}

/** Run the cases; return 0 when every check passes and 1 otherwise. */
int main(void)
{
    test_established();
    test_after_fin();
    test_data_past_fin();
    test_data_after_close();
    test_close_unread();
    test_shut_rd();
    test_shutdown_syn_sent();
    test_shutdown_states();
    test_shutdown_listener();
    test_forget_queued();
    test_syn_sent();
    test_syn_rcvd();
    test_syn_rcvd_segments();
    test_syn_rcvd_ack_data();
    test_syn_rcvd_simultaneous();
    test_syn_ack_resent();
    test_listener_gone();
    test_isn();
    if (fails == 0) {
        printf("tcp_reset_hosttest: PASS\n");
    }
    return fails != 0;
}
